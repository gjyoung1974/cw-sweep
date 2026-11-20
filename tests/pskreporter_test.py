#!/usr/bin/env python3
"""Tests --pskreporter: the datagrams cwsweep sends, read back by their own descriptors.

A stand-in server on the loopback interface collects the datagrams. cwsweep
plays, in real time, a recording of two stations around 14025 kHz, uploading
every 4 seconds. The second station gives its call some 20 seconds after the
first, so that the two go out in different datagrams.

  pskreporter_test.py CWSWEEP CWGEN WORKDIR
"""

import os
import socket
import struct
import subprocess
import sys
import threading
import time

cwsweep, cwgen, work = sys.argv[1:4]
os.makedirs(work, exist_ok=True)
wav = os.path.join(work, "psk_audio.wav")

subprocess.run([cwgen, "--seed", "5",
                "--station", "700:35:12:" + "   ".join(["CQ CQ DE K1ABC K1ABC K"] * 4),
                "--station", "1500:30:15:" + "VVV " * 6 + "   ".join(["CQ CQ DE W3MBO W3MBO K"] * 3), wav],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
srv.bind(("127.0.0.1", 0))
srv.settimeout(0.2)
port = srv.getsockname()[1]
datagrams, done = [], threading.Event()


def serve():
    while not done.is_set():
        try:
            datagrams.append(srv.recvfrom(4096))
        except socket.timeout:
            pass


threading.Thread(target=serve, daemon=True).start()
t_start = time.time()
p = subprocess.run([cwsweep, "--no-color", "--no-text", "--realtime", "--respot", "2", "--call", "n0xx",
                    "--grid", "fn42HN", "--antenna", "Dipole", "--freq", "14025k",
                    "--pskreporter-host", "127.0.0.1:%d" % port, "--pskreporter-interval", "4", wav],
                   stdout=subprocess.PIPE, text=True)
time.sleep(0.5)
done.set()
spotted = {line.split()[4] for line in p.stdout.splitlines() if line.startswith("DX de")}

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAILED:", what)


FIELDS = {0x8001: "senderCallsign", 0x8002: "receiverCallsign", 0x8004: "receiverLocator",
          0x8005: "frequency", 0x8006: "sNR", 0x8008: "decodingSoftware",
          0x8009: "antennaInformation", 0x800A: "mode", 0x800B: "informationSource",
          150: "flowStartSeconds"}


def parse_template(body, options):
    """-> (template id, [(name, length)])"""
    tid, count = struct.unpack(">HH", body[:4])
    pos = 6 if options else 4
    fields = []
    for _ in range(count):
        fid, flen = struct.unpack(">HH", body[pos:pos + 4])
        pos += 4
        if fid & 0x8000:
            check(struct.unpack(">I", body[pos:pos + 4])[0] == 30351, "enterprise number")
            pos += 4
        fields.append((FIELDS[fid], flen))
    check(all(b == 0 for b in body[pos:]) and len(body) - pos < 4, "descriptor padding")
    return tid, fields


def parse_records(body, fields):
    records, pos = [], 0
    while len(body) - pos >= 4 or any(body[pos:]):
        rec = {}
        for name, flen in fields:
            if flen == 0xFFFF:
                n = body[pos]
                rec[name] = body[pos + 1:pos + 1 + n].decode()
                pos += 1 + n
            else:
                rec[name] = int.from_bytes(body[pos:pos + flen], "big", signed=(name == "sNR"))
                pos += flen
        records.append(rec)
    return records


templates, reports, receivers, seqs, ports, session_ids = {}, [], [], [], set(), set()
with_templates = 0
for data, addr in datagrams:
    ports.add(addr[1])
    version, length, sent, seq, session = struct.unpack(">HHIII", data[:16])
    check(version == 10, "IPFIX version")
    check(length == len(data), "datagram length")
    check(abs(sent - time.time()) < 120, "transmission time")
    seqs.append(seq)
    session_ids.add(session)
    pos, n_records, had_template = 16, 0, False
    while pos < len(data):
        set_id, set_len = struct.unpack(">HH", data[pos:pos + 4])
        check(set_len % 4 == 0 and set_len >= 4, "set length is a multiple of 4")
        body = data[pos + 4:pos + set_len]
        pos += set_len
        if set_id in (2, 3):
            tid, fields = parse_template(body, set_id == 3)
            templates[tid] = fields
            had_template = True
        else:
            check(set_id in templates, "records follow their descriptor")
            recs = parse_records(body, templates.get(set_id, []))
            n_records += len(recs)
            (receivers if set_id == 0x9992 else reports).extend(recs)
    check(pos == len(data), "sets fill the datagram")
    with_templates += had_template
    seqs.append(n_records)

check(p.returncode == 0, "exit status")
check(spotted == {"K1ABC", "W3MBO"}, "both stations spotted: %s" % spotted)
check(len(datagrams) >= 2, "several uploads (%d)" % len(datagrams))
check(len(ports) == 1, "one source port")
check(len(session_ids) == 1, "one session identifier")
check(with_templates == min(len(datagrams), 3), "descriptors in the first three datagrams")
# The sequence number counts the records sent before each datagram.
check(seqs[0] == 1 and all(seqs[i + 2] == seqs[i] + seqs[i + 1] for i in range(0, len(seqs) - 2, 2)),
      "sequence numbers %s" % seqs)
check(len(receivers) == len(datagrams), "a receiver record in every datagram")
check(all(r == {"receiverCallsign": "N0XX", "receiverLocator": "FN42hn", "antennaInformation": "Dipole",
                "decodingSoftware": r["decodingSoftware"]} and r["decodingSoftware"].startswith("cwsweep ")
          for r in receivers), "receiver record %s" % receivers[:1])
check({r["senderCallsign"] for r in reports} == {"K1ABC", "W3MBO"}, "both stations reported")
# Respotted every few seconds, but reported once: one report per call, band and five minutes.
check(len(reports) == 2, "each station reported once (%d reports)" % len(reports))
freqs = {r["senderCallsign"]: r["frequency"] for r in reports}
check(abs(freqs.get("K1ABC", 0) - 14025700) <= 60, "K1ABC on 14025.7 kHz: %s" % freqs)
check(abs(freqs.get("W3MBO", 0) - 14026500) <= 60, "W3MBO on 14026.5 kHz: %s" % freqs)
check(all(r["mode"] == "CW" and r["informationSource"] == 1 for r in reports), "mode and source")
check(all(5 <= r["sNR"] <= 60 for r in reports), "signal reports %s" % [r["sNR"] for r in reports])
check(all(t_start - 2 <= r["flowStartSeconds"] <= time.time() + 2 for r in reports), "reception times")
print("pskreporter", "ok" if failures == 0 else "FAILED")
sys.exit(1 if failures else 0)
