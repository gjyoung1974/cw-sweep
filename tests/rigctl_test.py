#!/usr/bin/env python3
"""Tests --rigctl: frequency and mode following, and the pause while transmitting.

A stand-in rigctld reports a radio that transmits for the first 9 seconds,
sits on 14025 kHz in CW mode, and after 20 seconds moves to 7030 kHz USB.
cwsweep plays, in real time, a recording of one station at a 700 Hz pitch.

  rigctl_test.py CWSWEEP CWGEN WORKDIR
"""

import os
import socket
import subprocess
import sys
import threading
import time

cwsweep, cwgen, work = sys.argv[1:4]
os.makedirs(work, exist_ok=True)
wav = os.path.join(work, "rigctl_audio.wav")
TX_END, QSY = 9.0, 20.0

msg = "CQ CQ DE K1ABC K1ABC K"
subprocess.run([cwgen, "--seed", "5", "--station", "700:35:12:" + "   ".join([msg] * 5), wav],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 0))
srv.listen(1)
port = srv.getsockname()[1]
t0 = [None]


def serve():
    c, _ = srv.accept()
    t0[0] = time.time()
    buf = b""
    while True:
        try:
            d = c.recv(256)
        except OSError:
            return
        if not d:
            return
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = time.time() - t0[0]
            if line == b"f":
                c.sendall(b"14025000\n" if t < QSY else b"7030000\n")
            elif line == b"m":
                c.sendall(b"CW\n500\n" if t < QSY else b"USB\n2400\n")
            elif line == b"t":
                c.sendall(b"1\n" if t < TX_END else b"0\n")
            else:
                c.sendall(b"RPRT -11\n")


threading.Thread(target=serve, daemon=True).start()
p = subprocess.Popen([cwsweep, "--no-color", "--no-text", "--realtime", "--respot", "2", "--call", "N0XX",
                      "--rigctl", "127.0.0.1:%d" % port, wav], stdout=subprocess.PIPE, text=True)
spots = []
for line in p.stdout:
    if line.startswith("DX de"):
        spots.append((time.time() - t0[0], float(line.split()[3]), line.split()[4]))
p.wait()

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAILED:", what, spots)


check(p.returncode == 0, "exit status")
check(all(c == "K1ABC" for _, _, c in spots), "only K1ABC spotted")
check(all(t > TX_END for t, _, _ in spots), "nothing spotted while transmitting")
# CW mode: the 700 Hz tone is 100 Hz above a signal at the 600 Hz pitch.
check(any(f == 14025.1 for t, f, _ in spots if t < QSY), "spot on 14025.1 before the QSY")
check(any(f == 7030.7 for t, f, _ in spots if t > QSY), "spot on 7030.7 after the QSY")
check(all(f in (14025.1, 7030.7) for _, f, _ in spots), "no other frequencies")
check(all(f == 7030.7 for t, f, _ in spots if t > QSY + 1), "old frequency not reported after the QSY")
print("rigctl", "ok" if failures == 0 else "FAILED")
sys.exit(1 if failures else 0)
