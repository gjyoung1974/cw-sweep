#!/usr/bin/env python3
"""A stand-in for an openHPSDR protocol 1 radio (Hermes Lite 2), for testing
cwsweep's hpsdr source without hardware.

Answers discovery, and after a start packet streams a 2-channel I/Q WAV file
as receiver 1. Every command packet the host sends is checked: when the host
stops (or the file ends) a JSON summary is printed, including whether any
packet would have keyed a transmitter.

  fake_hpsdr.py --port 10240 --speed 4 band_iq.wav

Prints READY once it is listening.
"""

import argparse
import json
import select
import socket
import struct
import sys
import time

RATES = {0: 48000, 1: 96000, 2: 192000, 3: 384000}


def read_wav(path):
    """Returns (interleaved I/Q as 24-bit integers, sample rate). 16-bit PCM or 32-bit float."""
    d = open(path, "rb").read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        sys.exit("not a WAV file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        tag, size = d[pos:pos + 4], struct.unpack("<I", d[pos + 4:pos + 8])[0]
        body = d[pos + 8:pos + 8 + size]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            data = body
        pos += 8 + size + (size & 1)
    if fmt is None or data is None or fmt[1] != 2:
        sys.exit("need a 2-channel I/Q WAV file")
    kind, bits = fmt[0], fmt[5]
    if kind == 1 and bits == 16:
        vals = [v * 256 for v in struct.unpack("<%dh" % (len(data) // 2), data)]
    elif kind == 3 and bits == 32:
        fl = struct.unpack("<%df" % (len(data) // 4), data)
        peak = max(1e-9, max(abs(v) for v in fl))
        k = 0.25 * 8388607 / peak
        vals = [int(v * k) for v in fl]
    else:
        sys.exit("need 16-bit PCM or 32-bit float samples")
    return vals, fmt[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--port", type=int, default=10240)
    ap.add_argument("--speed", type=float, default=1.0, help="playback speed, x real time")
    ap.add_argument("--busy", action="store_true", help="report that another program is streaming")
    ap.add_argument("--drop", type=int, default=0, help="drop every Nth data packet")
    ap.add_argument("--timeout", type=float, default=20.0, help="give up if no host starts")
    a = ap.parse_args()

    vals, wav_rate = read_wav(a.wav)
    nsamp = len(vals) // 2
    # One 8-byte sample set per sample: I and Q as 24-bit big-endian, then the microphone.
    sets = b"".join((v & 0xFFFFFF).to_bytes(3, "big") + (b"\x00\x00" if k & 1 else b"")
                    for k, v in enumerate(vals))

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", a.port))
    print("READY", flush=True)
    info = {"discoveries": 0, "starts": 0, "commands": 0, "bad": [], "mox": 0, "nonzero_payload": 0,
            "addresses": {}, "rate": None, "duplex": None, "receivers": None, "stopped": False,
            "wav_rate": wav_rate}

    def reply_discovery(addr):
        r = bytearray(60)
        r[0:3] = b"\xef\xfe" + (b"\x03" if a.busy else b"\x02")
        r[3:9] = bytes([0x00, 0x1C, 0xC0, 0xA2, 0x12, 0x34])
        r[9] = 74      # gateware major
        r[10] = 6      # Hermes Lite
        r[19] = 4      # receivers
        r[21] = 2      # gateware minor
        s.sendto(r, addr)

    def handle(pkt, addr):
        if len(pkt) == 63 and pkt[:3] == b"\xef\xfe\x02":
            info["discoveries"] += 1
            reply_discovery(addr)
            return None
        if len(pkt) == 64 and pkt[:3] == b"\xef\xfe\x04":
            if pkt[3] == 0x01:
                info["starts"] += 1
                return "start"
            if pkt[3] == 0x00:
                info["stopped"] = True
                return "stop"
            info["bad"].append("start/stop byte %#x" % pkt[3])
            return None
        if len(pkt) == 1032 and pkt[:4] == b"\xef\xfe\x01\x02":
            info["commands"] += 1
            for base in (8, 520):
                f = pkt[base:base + 512]
                if f[:3] != b"\x7f\x7f\x7f":
                    info["bad"].append("sync")
                    continue
                c0, data = f[3], f[4:8]
                if c0 & 1:
                    info["mox"] += 1
                if any(f[8:]):
                    info["nonzero_payload"] += 1
                info["addresses"]["%#04x" % c0] = data.hex()
                if c0 == 0x00:
                    info["rate"] = RATES[data[0] & 3]
                    info["duplex"] = bool(data[3] & 0x04)
                    info["receivers"] = ((data[3] >> 3) & 0x0F) + 1
            return None
        info["bad"].append("unknown packet, %d bytes" % len(pkt))
        return None

    # Wait for a host.
    host = None
    deadline = time.time() + a.timeout
    while host is None and time.time() < deadline:
        if select.select([s], [], [], 0.2)[0]:
            pkt, addr = s.recvfrom(2048)
            if handle(pkt, addr) == "start" and not a.busy:
                host = addr

    seq = 0
    pos = 0
    radd = 0
    t0 = time.time()
    while host is not None and pos + 126 <= nsamp and not info["stopped"]:
        rate = info["rate"] or 48000
        pkt = bytearray(b"\xef\xfe\x01\x06" + struct.pack(">I", seq & 0xFFFFF))
        for _ in range(2):
            # Status as a Hermes Lite sends it: RADDR cycles 0..3, no PTT, no overload.
            pkt += b"\x7f\x7f\x7f" + bytes([radd << 3, 0x1E, 0, 0, 74])
            radd = (radd + 1) % 4
            pkt += sets[8 * pos:8 * (pos + 63)]
            pos += 63
        seq += 1
        if not (a.drop and seq % a.drop == 0):
            s.sendto(pkt, host)
        due = t0 + pos / (rate * a.speed)
        while True:
            wait = due - time.time()
            if not select.select([s], [], [], max(0.0, wait))[0]:
                break
            p, addr = s.recvfrom(2048)
            handle(p, addr)
            if wait <= 0:
                break

    # Let the host finish and send its stop.
    end = time.time() + 8.0
    while not info["stopped"] and host is not None and time.time() < end:
        if select.select([s], [], [], 0.2)[0]:
            p, addr = s.recvfrom(2048)
            handle(p, addr)
    info["sent_packets"] = seq
    print(json.dumps(info))


if __name__ == "__main__":
    main()
