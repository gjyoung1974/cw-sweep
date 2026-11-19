# TODO

Work left over from making cwsweep friendlier to openHPSDR radios (Hermes
Lite 2, Radioberry). See the "Limitations and ideas" section of the README for
the wider list.

## Take I/Q from the SDR program (TCI)

Only one program can own an openHPSDR radio, so cwsweep can't use the `hpsdr`
source while Thetis, SparkSDR or piHPSDR is running. Today the only way to run
alongside one is its audio plus `--rigctl`, which covers about 3 kHz.

- Add a `tci[:HOST[:PORT]]` source: I/Q and the centre frequency over one
  websocket (Thetis, SparkSDR).
- Follow frequency and transmit state from the same connection, instead of
  `--rigctl`.
- Push spots back to the program's panadapter with TCI's spot command.
- piHPSDR has no I/Q output as far as is known; check, and document the audio
  route for it if so.

## Several receivers and bands in one process

A Hermes Lite 2 can stream several receivers at once (4 on the stock
gateware, more on some variants); cwsweep uses one, on one band.

- Ask the radio for N receivers and split the sample sets per receiver
  (`hpsdr.cpp` assumes one: 63 sample sets per frame).
- One engine per receiver, each on its own thread, all feeding one telnet
  server.
- A way to say it on the command line, e.g. `--band 40m,20m,15m`.
- Threading matters most on a Radioberry, where everything shares a
  Raspberry Pi. The engine itself is still single-threaded.

## Config file with named profiles

Command lines get long (call, telnet port, band, gain, SCP file, source).

- Read settings from a file, e.g. `~/.config/cwsweep/cwsweep.conf`, with
  named profiles: `cwsweep --profile hl2-40m`.
- Command-line options override the file.
- Let the systemd units point at a profile instead of `CWSWEEP_ARGS`.

## Frequency calibration (`--ppm`)

No correction for the radio's reference oscillator error. Add `--ppm` (or
`--freq-offset HZ`) and apply it to reported frequencies; for `hpsdr`, apply
it to the tuning frequency sent to the radio.

## arm64 package

No packaging. A Radioberry runs on a Raspberry Pi, so an arm64 `.deb` (or at
least a tested build recipe for Raspberry Pi OS) would save building on the
Pi. `-DCWSWEEP_NATIVE=OFF` is needed for a portable binary.

## Verify on real hardware

The `hpsdr` source has only been tested against `tests/fake_hpsdr.py`.

- Run it on a Hermes Lite 2 and on a Radioberry.
- Confirm the spectrum isn't mirrored (otherwise swap I and Q by default).
- Confirm the watchdog is kept fed at every sample rate, and that the radio
  stops cleanly when cwsweep exits.
- A Radioberry always reports itself idle, so the "in use by another
  program" check can't protect it; find another way to detect a running
  client, or document it.
- Test `--rigctl` transmit detection against real radios: Hamlib's dummy
  radio doesn't report transmit state reliably.
