#!/usr/bin/env python3
"""End-to-end test of cwsweep's hpsdr source against tests/fake_hpsdr.py.

  hpsdr_test.py CWSWEEP CWGEN WORKDIR
"""

import json
import os
import subprocess
import sys

cwsweep, cwgen, work = sys.argv[1:4]
here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(work, exist_ok=True)
wav = os.path.join(work, "hpsdr_iq.wav")
failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAILED:", what)


def run(port, fake_args, sweep_args):
    fake = subprocess.Popen([sys.executable, os.path.join(here, "fake_hpsdr.py"), "--port", str(port)] +
                            fake_args + [wav], stdout=subprocess.PIPE, text=True)
    assert fake.stdout.readline().strip() == "READY"
    p = subprocess.run([cwsweep, "--no-color"] + sweep_args + ["hpsdr:127.0.0.1:%d" % port],
                       capture_output=True, text=True, timeout=60)
    summary = json.loads(fake.communicate(timeout=30)[0])
    spots = {}
    for line in p.stdout.splitlines():
        if line.startswith("DX de"):
            f = line.split()
            spots[f[4]] = float(f[3])
    return p, spots, summary


# Stations 2 kHz above, 8 kHz below and 20 kHz above 7025 kHz.
subprocess.run([cwgen, "--fs", "48000", "--iq", "--dur", "14", "--seed", "3",
                "--station", "2000:25:10:CQ CQ DE K1ABC K1ABC K",
                "--station", "-8000:30:8:CQ TEST DL1AA DL1AA TEST",
                "--station", "20000:22:10:CQ CQ DE W9XYZ W9XYZ K", wav],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# 1. A 30 kHz segment: centred, at the lowest rate that covers it.
p, spots, s = run(10311, ["--speed", "4"], ["--segment", "7010k-7040k", "--call", "N0XX"])
check(p.returncode == 0, "exit status %d: %s" % (p.returncode, p.stderr))
check(spots.get("K1ABC") == 7027.0, "K1ABC on 7027.0: %r" % spots)
check(spots.get("DL1AA") == 7017.0, "DL1AA on 7017.0: %r" % spots)
check("W9XYZ" not in spots, "W9XYZ is outside the segment: %r" % spots)
check(s["rate"] == 48000 and s["duplex"] and s["receivers"] == 1, "general settings: %r" % s)
check(s["addresses"].get("0x04") == "%08x" % 7025000, "receiver frequency: %r" % s["addresses"])
check(s["addresses"].get("0x14") == "00000060", "LNA gain +20 dB: %r" % s["addresses"])
check(s["addresses"].get("0x12") == "00040000", "drive 0, PA off: %r" % s["addresses"])
# Nothing sent may key a transmitter.
check(s["mox"] == 0 and s["nonzero_payload"] == 0, "transmit bits or samples sent: %r" % s)
check(set(s["addresses"]) == {"0x00", "0x02", "0x04", "0x12", "0x14"}, "addresses written: %r" % s["addresses"])
check(not s["bad"], "malformed packets: %r" % s["bad"])
check(s["stopped"], "no stop packet at exit")

# 2. Own call is never spotted; lost packets are reported; gain is passed on.
p, spots, s = run(10312, ["--speed", "4", "--drop", "300"],
                  ["--segment", "7010k-7040k", "--call", "K1ABC", "--gain", "-3"])
check("K1ABC" not in spots and "DL1AA" in spots, "own call filtered: %r" % spots)
check("packets lost" in p.stderr, "lost packets reported: %s" % p.stderr)
check(s["addresses"].get("0x14") == "00000049", "LNA gain -3 dB: %r" % s["addresses"])

# 3. A radio another program is using is left alone.
p, spots, s = run(10313, ["--busy", "--timeout", "3"], ["--band", "40m"])
check(p.returncode == 1 and "in use" in p.stderr, "busy radio refused: %s" % p.stderr)
check(s["starts"] == 0 and s["commands"] == 0, "busy radio was sent commands: %r" % s)

print("hpsdr", "ok" if failures == 0 else "FAILED")
sys.exit(1 if failures else 0)
