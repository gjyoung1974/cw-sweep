# Running cwsweep as a daemon and uploading spots

cwsweep is designed to run unattended:

- it exits cleanly on SIGTERM;
- it flushes every output line, so logs are live;
- it exits when its input fails (SDR unplugged, pipe closed), so a service
  manager can restart it.

The recommended setup is two systemd services:

```
radio / SDR ──► cwsweep ──► telnet :7300 ──┬─► your logger (N1MM+, DXLog, CC-User, ...)
                  (spots in DX-cluster      └─► cwsweep-forward ──┬─► a DX-cluster node
                   format)                                        ├─► an HTTP endpoint (JSON)
                                                                  └─► a JSON-lines log file
```

- **cwsweep** decodes and spots, and serves spots on its telnet port.
- **cwsweep-forward** is a small relay (Python 3, standard library only). It
  connects to that port as a client and uploads each spot. The two restart
  independently: the relay reconnects with back-off if cwsweep or the
  destination goes away.

## 1. Install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build
```

This installs `cwsweep`, `cwsweep-forward`, `cwgen` and `cwbench` to
`/usr/local/bin`. The example unit and settings files go to
`/usr/local/share/cwsweep/systemd/`, and this document to
`/usr/local/share/cwsweep/`.

Before making it a service, check your command line interactively. Make sure
spots appear and the frequencies are right (`--freq` must be the dial or
centre frequency):

```sh
cwsweep --call N0CALL --telnet 7300 --freq 14.06M --rate 192k soapy:driver=rtlsdr,direct_samp=2
cwsweep --call N0CALL --telnet 7300 --band 40m hpsdr      # Hermes Lite 2 / Radioberry
```

Give your real callsign to `--call`: it names the spotter, and it is never
spotted itself, which matters when the receiver hears your own transmitter.

## 2a. System service: SDR, pipe or file input

Use this when the input is a SoapySDR device, a Hermes Lite 2 or Radioberry
(`hpsdr`), a raw pipe or a file. It starts at boot, with no login needed.

```sh
sudo useradd --system --home-dir /var/lib/cwsweep --shell /usr/sbin/nologin cwsweep
sudo cp /usr/local/share/cwsweep/systemd/cwsweep.service /etc/systemd/system/
sudo cp /usr/local/share/cwsweep/systemd/cwsweep.env.example /etc/default/cwsweep
sudoedit /etc/default/cwsweep          # set CWSWEEP_ARGS: your call, --freq, the source
sudo systemctl daemon-reload
sudo systemctl enable --now cwsweep
```

A Hermes Lite 2 or Radioberry is reached over the network (`--band 40m
hpsdr`), so it needs no device permissions. The unit must start after the
network is up; if the radio is found only some of the time at boot, give its
address (`hpsdr:192.168.1.50`) instead of relying on the discovery broadcast.

For a USB SDR, the `cwsweep` user needs access to the device. Either:

- install the udev rules that ship with your SDR's driver package; or
- uncomment `SupplementaryGroups=` in the unit with the right group
  (`plugdev` on Debian/Ubuntu), then run
  `sudo systemctl daemon-reload && sudo systemctl restart cwsweep`.

Feeding from another program, such as `rtl_sdr`, needs a shell pipeline in
`ExecStart`. Use a drop-in (`sudo systemctl edit cwsweep`), for example:

```ini
[Service]
ExecStart=
ExecStart=/bin/sh -c 'rtl_sdr -f 7030000 -s 240000 -D 2 - | exec /usr/local/bin/cwsweep --no-color --iq --format u8 --rate 240k --freq 7.03M - $CWSWEEP_ARGS'
```

The empty `ExecStart=` line clears the original. Note that `$CWSWEEP_ARGS`
now no longer needs a source.

## 2b. User service: sound-card input

PulseAudio and PipeWire run per user, so a system service can't open the
sound card. Run cwsweep as a user service instead, and enable *lingering* so
it starts at boot without anyone logged in:

```sh
mkdir -p ~/.config/systemd/user ~/.config/cwsweep
cp /usr/local/share/cwsweep/systemd/user/cwsweep.service ~/.config/systemd/user/
cp /usr/local/share/cwsweep/systemd/cwsweep.env.example ~/.config/cwsweep/cwsweep.env
pactl list short sources                # find the radio's sound card name
$EDITOR ~/.config/cwsweep/cwsweep.env   # e.g. CWSWEEP_ARGS=... pulse:alsa_input.usb-...
systemctl --user daemon-reload
systemctl --user enable --now cwsweep
sudo loginctl enable-linger "$USER"     # keep user services running without a login
```

Use `systemctl --user` and `journalctl --user` for the commands in the rest
of this document.

With a fixed `--freq`, spots are wrong as soon as the radio is tuned. If the
radio (or the SDR program driving it) is reachable through Hamlib's
`rigctld`, add `--rigctl` and drop `--freq`:

```sh
CWSWEEP_ARGS=--call N0CALL --telnet 7300 --no-text --rigctl localhost pulse:alsa_input.usb-...
```

cwsweep then follows the dial frequency and mode, and stops decoding while
the radio transmits. It keeps running, and reconnects, if `rigctld` isn't up
yet or goes away. Until the first answer, text is shown with audio offsets
and no spots are made. For a radio in CW mode, set
`--pitch` to its sidetone pitch (600 Hz is assumed).

## 3. Check that it works

```sh
systemctl status cwsweep
journalctl -u cwsweep -f                # spots as they are made (--user -u for 2b)
telnet localhost 7300                   # enter a callsign; spots stream in
```

## 4. Uploading spots

### Your logger

Point the logger's telnet/DX-cluster window at `<host>:7300` and log in with
any callsign. Spot lines use the Reverse Beacon Network column layout,
which loggers parse by position.

The telnet server has no authentication and listens on all interfaces. Either
firewall the port to your own network, or reach it over an SSH tunnel:

```sh
ssh -L 7300:localhost:7300 shack-pc
```

### A DX-cluster node

```sh
sudo cp /usr/local/share/cwsweep/systemd/cwsweep-forward.service /etc/systemd/system/
sudo cp /usr/local/share/cwsweep/systemd/cwsweep-forward.env.example /etc/default/cwsweep-forward
sudoedit /etc/default/cwsweep-forward
sudo systemctl daemon-reload
sudo systemctl enable --now cwsweep-forward
```

For example:

```sh
FORWARD_ARGS=--from localhost:7300 --login N0CALL --cluster dxc.example.org:7300 --min-interval 2
```

The relay logs in to the node with your callsign, plus `--cluster-password`
if it asks for one. It then posts each spot with the standard cluster
command:

```
DX 14025.3 K1ABC CW 18 dB 24 WPM CQ
```

Most DXSpider, AR-Cluster and CC Cluster nodes accept this command, but
there's no common standard; test against yours first.

**Ask the node's sysop before connecting an automatic spotter.** Many public
clusters don't allow unattended "skimmer" spots on their main feed, because
they can flood it. Suitable choices are your own node (DXSpider runs well on
a Raspberry Pi), or a node whose sysop has agreed and perhaps gives you a
separate login. To keep volume reasonable:

- `--min-interval` sets the minimum seconds between posts (default 2);
- `--only-cq` forwards only stations calling CQ;
- cwsweep's `--respot SEC` (default 600) stops the same call being spotted
  repeatedly on the same frequency.

### An HTTP endpoint or a log file

`--http URL` POSTs each spot as JSON. `--jsonl FILE` appends one JSON object
per line:

```json
{"spotter": "N0CALL-#", "freq_khz": 7025.7, "call": "W3MBO", "comment": "CW 17 dB 19 WPM CQ",
 "time": "1830Z", "snr_db": 17, "wpm": 19, "kind": "CQ",
 "raw": "DX de N0CALL-#:   7025.7  W3MBO        CW    17 dB  19 WPM  CQ        1830Z"}
```

Use `--http` for a dashboard, a database, or a chat bot: anything that
accepts a webhook. HTTP errors are logged and the spot is dropped. They
never stop the relay.

The destinations can be combined; give several of `--cluster`, `--http` and
`--jsonl`. Use `--dry-run` to print the cluster commands that would be sent,
without connecting anywhere.

Without the telnet server, spots can also be piped straight in:

```sh
cwsweep --no-text --freq 7.025M band.wav | cwsweep-forward --stdin --jsonl spots.jsonl
```

### The Reverse Beacon Network

The RBN collects spots through its *Aggregator* program (Windows; it can run
under Wine). The Aggregator connects to a skimmer's telnet port and accepts
spots only from skimmer software the RBN supports. cwsweep's telnet output
uses the RBN spot format, but it has **not** been tested with the
Aggregator, and cwsweep isn't one of the RBN's supported skimmers. Contact
the RBN team (reversebeacon.net) before trying to feed it. Spots from
unvetted decoders affect a network many operators rely on.

## 5. Operations

| Task | Command |
|---|---|
| live log | `journalctl -u cwsweep -f` |
| spots made today | `journalctl -u cwsweep --since today \| grep 'DX de'` |
| change settings | edit the env file, then `systemctl restart cwsweep` |
| stop / disable | `systemctl disable --now cwsweep cwsweep-forward` |
| upgrade | rebuild, `sudo cmake --install build`, `systemctl restart cwsweep cwsweep-forward` |

Troubleshooting:

- **Restarts every few seconds.** Read the error in `journalctl -u cwsweep`.
  The usual causes are no permission on the SDR (see 2a), a wrong sound-card
  name, or the telnet port already in use.
- **`PulseAudio: Connection refused` from the system service.** Sound-card
  input needs the user service (2b).
- **`hpsdr: ... is in use by another program`.** Only one program can use an
  openHPSDR radio. Close the SDR program, or run cwsweep on its audio with
  `--rigctl` instead.
- **`hpsdr: ADC overload`** in the log. Lower `--gain`.
- **`rigctld at ...: cannot connect; retrying`.** `rigctld` isn't running, or
  listens on another port (`--rigctl HOST:PORT`, default 4532).
- **Spots a few hundred Hz off with `--rigctl`, radio in CW mode.** Set
  `--pitch` to the radio's sidetone pitch. If they are on the wrong side of
  the dial, add `--lsb`.
- **Spots on the wrong frequency.** `--freq` must be the radio's dial
  frequency (audio input) or the SDR's centre frequency (I/Q). Add `--lsb` if
  the radio is in LSB or CW-reverse mode.
- **Too many or too few spots.** `--threshold` (dB, default 4) controls how
  weak a signal is decoded. `--scp MASTER.SCP` allows single-copy spots of
  known calls. Without it, a call must be copied twice.
