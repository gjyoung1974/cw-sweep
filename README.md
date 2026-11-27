# cwsweep

A multi-channel CW (Morse code) decoder and DX-cluster spotter for Linux and
other Unix systems. It decodes **every** CW signal in the receiver passband at
once (3 kHz of radio audio, or a wide I/Q band from an SDR), extracts
callsigns, and serves them as DX spots to loggers over telnet.

It is a command-line program written in C++20. The decoder is built around
Bayesian / maximum-likelihood methods aimed at weak, noisy, fading signals and
real human fists.

```
$ cwsweep --freq 7.025M --telnet 7300 pulse
   7025.80  32 WPM  +15 dB  CQ TEST PY0X PY0X
DX de N0CALL-#:   7025.8  PY0X         CW    15 dB  32 WPM  CQ        1532Z
   7027.53  34 WPM  +13 dB  CQ CQ DE M2ZB M2ZB K
DX de N0CALL-#:   7027.5  M2ZB         CW    13 dB  34 WPM  CQ        1532Z
   7027.18  35 WPM  +12 dB  F2TL DE S5H R TNX FER CALL UR RST 599 NAME KEN QTH MA HW? F2TL DE
```

## How it works

```
samples ─► impulse noise blanker ─► STFT channelizer (50 Hz resolution, 25 Hz bins, 5 ms frames)
        ─► per-bin noise floor + signal detection (every bin, 5x per second)
        ─► per channel:
             matched filter: coherent sum of phase-aligned STFT frames, length tuned to the dit, with AFC
             Rician likelihood ratio per frame, marginalised over signal level (QSB)
             hard path: fixed-lag Viterbi keyer ─► run lengths ─► speed / weight / fist estimation
             soft path: explicit-duration HMM beam search over the per-frame likelihoods ─► text
        ─► callsign extraction ─► spot policy ─► stdout / telnet DX cluster
```

With `--rigctl`, a retune or a change of mode flushes every channel and
discards the signal history, so nothing heard on the old frequency is
reported on the new one. Samples received while the radio transmits are
dropped, and decoding restarts from a clean history afterwards.

* **Channelizer and detection.** One FFT covers the whole passband: 7,680
  bins, 25 Hz apart, for a 192 kHz I/Q stream. Each bin's noise floor comes from order
  statistics: the 20th percentile sits in a keyed signal's gaps, and a median
  across neighbouring bins makes it robust in a crowded band. A station is a
  local peak whose 85th percentile is above threshold on two consecutive
  passes. The last 2 seconds are replayed into a new channel, so the start of
  a transmission isn't lost.
* **Receiver passband.** The noise floor is only estimated, and stations are
  only looked for, inside the receiver's passband: the contiguous stretch of
  bins carrying band noise. With a narrow CW filter in front of a sound card
  most of the 200–3500 Hz range holds no noise at all, which would otherwise
  put the floor far too low for the signals inside the filter.
* **Harmonic images.** For audio input, a candidate at 2–5 times the pitch of
  a much stronger signal that keys in step with it is taken to be a harmonic
  from an overdriven audio stage, and is not decoded or spotted.
* **Matched filter.** Once a channel's speed is known, its filter becomes a
  coherent sum of K phase-aligned STFT frames. That gives a Hann window
  convolved with a boxcar, sized to the shortest element (dit or gap,
  allowing for keying weight). It has low sidelobes, so strong neighbours
  don't leak into the gaps, and frequency is tracked by AFC from the phase
  advance between frames.
* **Likelihoods.** Per frame, the exact log-likelihood ratio for a Rician
  carrier in Gaussian noise, `−S/N + ln I0(2√(S·P)/N)`. It is averaged over a
  range of signal levels, so a mark that has faded 10 dB is still a mark.
  Fade levels that can't be told apart from noise are excluded.
* **Timing model.** Dit length, keying weight and the sender's timing spread
  are fitted continuously to recent element durations by a robust grid
  search, with class priors and hysteresis. Letter and word gaps are fitted
  per station as well: operators who leave long gaps between letters
  (deliberate hand sending, Farnsworth-style practice transmissions) are read
  as words, not one letter per word. Decoding of a new station starts once
  its spacing has been seen, about a word in, and replays from its first
  character.
* **Soft-decision decoder.** A beam search over hypotheses that each carry the
  key state, the time spent in it, the Morse pattern so far and the text not
  yet committed. When a run ends, its length is scored against the dit/dah or
  element/character/word-gap duration model. Patterns are constrained to
  valid Morse prefixes, and a weak character-bigram language model trained on
  amateur traffic breaks ties. This recovers dahs split by noise and dits
  merged by it, which a decide-then-classify decoder cannot. Text is
  committed with a short fixed lag.
* **Spotting.** A callsign is spotted only after two identical copies on the
  same channel. With a `MASTER.SCP` file (`--scp`), a single copy also counts
  if it follows CQ/DE/TEST/TU and the call is in the list. Spot lines use the
  Reverse Beacon Network (RBN) column layout that loggers expect.
* **Noise blanker.** Samples are delayed by about 13 ms so that each
  above-threshold excursion can be measured before it is output. Only short
  excursions (under 8 ms) are blanked. Strong stations, and the beat patterns
  between them, are never chopped, because every CW element is longer than
  that.

## Building

Requirements: CMake ≥ 3.20, a C++20 compiler, FFTW3 (single precision) and
libsndfile. PulseAudio/PipeWire and SoapySDR support are built in when found.

```sh
# Debian/Ubuntu
sudo apt install cmake g++ libfftw3-dev libsndfile1-dev libpulse-dev libsoapysdr-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build            # unit + end-to-end tests (about a minute)
sudo cmake --install build        # optional: cwsweep, cwsend, cwgen, cwbench -> /usr/local/bin
```

The openHPSDR (Hermes Lite 2, Radioberry) source and the `rigctld` follower
need no extra libraries. Their tests run against stand-ins on the loopback
interface and need Python 3; without it they are skipped.

Options: `-DCWSWEEP_PULSE=OFF`, `-DCWSWEEP_SOAPY=OFF`, `-DCWSWEEP_NATIVE=OFF` (don't use
`-march=native`, for portable binaries).

## Usage

```
cwsweep [options] SOURCE
```

| SOURCE | |
|---|---|
| `FILE.wav` | any file libsndfile reads: mono audio, or stereo I/Q with `--iq` |
| `-` or `raw:PATH` | raw samples from stdin, a file or a FIFO (`--rate`, `--format s16\|f32\|u8`) |
| `pulse[:DEVICE]` | PulseAudio/PipeWire capture: the radio's audio via a USB sound card or virtual cable |
| `soapy[:ARGS]` | SoapySDR device (RTL-SDR, Airspy HF+, SDRplay, HackRF, ...); needs `--freq` |
| `hpsdr[:ADDRESS]` | Hermes Lite 2, Radioberry or another openHPSDR protocol 1 radio on the network; needs `--band`, `--segment` or `--freq` |

### Examples

Radio audio (3 kHz, USB/CW mode) from a USB sound card, spots to a logger:

```sh
pactl list short sources                       # find the device name
cwsweep --freq 14.025M --call N0CALL --telnet 7300 pulse:alsa_input.usb-Burr-Brown_USB_Audio_CODEC-00.analog-stereo
```

Wideband I/Q from a SoftRock-style sound-card SDR (192 kHz stereo):

```sh
cwsweep --iq --rate 192k --freq 7.05M pulse:alsa_input.pci-0000_00_1f.3.analog-stereo
```

Hermes Lite 2 or Radioberry, with cwsweep as the only program using the
radio. The radio is found on the network, tuned to the middle of the CW
segment and run at the lowest sample rate that covers it:

```sh
cwsweep --discover                              # which radios are out there?
cwsweep --band 40m --call N0CALL --telnet 7300 hpsdr
cwsweep --segment 14000k-14100k --gain 14 hpsdr:192.168.1.50
```

Only one program can use such a radio at a time, so this is for a dedicated
skimmer receiver; cwsweep refuses a radio that is already streaming to
another program. It only receives: it never sets the transmit bit, and keeps
the drive level at zero and the power amplifier off. `--gain` is the LNA gain,
−12 to +48 dB (default 20); lower it if cwsweep reports ADC overload.

Alongside your own operating, on the radio's audio: follow the dial through
Hamlib's `rigctld`, so spots stay on frequency as you tune and decoding
pauses while you transmit:

```sh
rigctld -m <model> -r <port> &                  # or your SDR program's rigctl server
cwsweep --rigctl localhost --call N0CALL --telnet 7300 pulse
```

USB and LSB are followed automatically. In CW mode the audio pitch of a
signal on the dial frequency matters: set `--pitch` to the radio's sidetone
pitch (600 Hz is assumed). If CW-mode spots land on the wrong side of the
dial, add `--lsb`, which swaps the sideband sense.

SoapySDR device, decoding about ±90 kHz around 14.06 MHz:

```sh
cwsweep --freq 14.06M --rate 192k --gain 30 soapy:driver=sdrplay
cwsweep --freq 7.03M --rate 240k soapy:driver=rtlsdr,direct_samp=2   # RTL-SDR in direct-sampling HF mode
```

Piping from other tools:

```sh
rtl_sdr -f 7030000 -s 240000 -D 2 - | cwsweep - --iq --format u8 --rate 240k --freq 7.03M
arecord -f S16_LE -r 12000 -c 1 -t raw | cwsweep - --rate 12000 --freq 3.53M
```

Record now, replay later (works with any of the sources above):

```sh
cwsweep --record band.wav --freq 7.025M pulse
cwsweep --freq 7.025M --realtime band.wav
```

Live channel table instead of scrolling text: `--tui`.

### Options

| Option | |
|---|---|
| `--iq`, `--swap-iq` | input is I/Q; swap I and Q |
| `--rate HZ` | sample rate for raw/pulse/soapy/hpsdr (`k`/`M` suffixes accepted) |
| `--band NAME` | decode the CW part of a band (`160m` ... `6m`); for SDRs also sets the centre frequency |
| `--segment LO-HI` | decode this frequency range instead, e.g. `7000k-7040k` |
| `--gain DB` | SoapySDR gain (default automatic); hpsdr LNA gain (default 20) |
| `--rigctl HOST[:PORT]` | follow frequency and mode through `rigctld`; pause while transmitting |
| `--pitch HZ` | audio pitch of a signal on the dial frequency, radio in CW mode |
| `--discover` | list the openHPSDR radios on the network |
| `--freq F` | dial frequency (audio) or centre frequency (I/Q), e.g. `14025k`, `7.03M` |
| `--lsb` | audio from a radio in LSB / CW-reverse mode |
| `--fmin`, `--fmax` | offset range to decode (audio default 200–3500 Hz; I/Q: whole band) |
| `--threshold DB` | detection threshold, signal/noise in one bin (default 4) |
| `--no-nb`, `--nb-threshold X` | noise blanker off / threshold (default 8× background) |
| `--lm W` | language-model weight; 0 disables (default 0.4) |
| `--call CALL` | your callsign: names the spotter (`CALL-#`, like the RBN) and is never spotted itself |
| `--telnet PORT` | DX-cluster telnet server |
| `--pskreporter` | upload spots to [PSK Reporter](https://pskreporter.info), batched every five minutes; needs `--call` and `--grid` |
| `--grid LOCATOR`, `--antenna TEXT` | your Maidenhead locator (`FN42` or `FN42hn`) and antenna, for PSK Reporter |
| `--scp FILE` | known-calls list (`MASTER.SCP`) for single-copy spotting |
| `--respot SEC` | minimum time before re-spotting a call near the same frequency (600) |
| `--tui`, `--no-text`, `--no-spots`, `--min-snr DB` | output selection |
| `--all-text` | also show text that looks like decoded noise (hidden by default) |
| `--record FILE.wav`, `--realtime` | record input; play files at real-time speed |

Connecting a logger: point N1MM+, DXLog, CC-User, etc. at a telnet cluster on
`<host>:7300`, or try it by hand with `telnet localhost 7300`. Send any
callsign at the login prompt.

### Sending CW: cwsend

`cwsend` is a separate program that sends CW through a Radioberry, Hermes
Lite 2 or other openHPSDR protocol 1 radio. Give it text and it sends that
and exits; give it none and it sends every line it reads from standard input:

```sh
cwsend --freq 7030k --wpm 22 cq cq de n0call n0call k
cwsend --freq 7030k --radio 192.168.1.50        # then type:
cq cq de n0call n0call k                        # sent on 7030 kHz
/wpm 28                                         # change the speed
/freq 7032.5k                                   # change the transmit frequency
/stop                                           # stop sending, forget what is queued
```

| Option | |
|---|---|
| `--freq F` | transmit frequency (required) |
| `--wpm N` | speed, 5–60 (default 20) |
| `--drive N` | the radio's drive level, 0–255 (default 255, full power) |
| `--radio ADDRESS[:PORT]` | the radio; found by broadcast if not given |
| `--discover` | list the openHPSDR radios on the network |

`<AR>`, `<BT>`, `<KN>`, `<SK>` and `<AS>` are sent as prosigns; characters with
no Morse code are left out. Lines typed while one is being sent are queued,
and at the end of the input (Ctrl-D) cwsend finishes what is queued and exits.
The carrier is exactly on the transmit frequency, keyed with 5 ms
raised-cosine edges; the transmitter is switched on 30 ms before the first
element and released 300 ms after the last. No band filters are selected:
low-pass filtering after the radio is yours to arrange, and you need a
licence for the frequency.

Only one program can use the radio at a time, so cwsend can't run while
`cwsweep ... hpsdr` or an SDR program has it.

### Running as a service and uploading spots

[docs/daemon.md](docs/daemon.md) covers:

- running cwsweep under systemd, as a system service (SDR, Hermes Lite 2 /
  Radioberry, pipe) or a user service (sound card, with `--rigctl` to follow
  the radio);
- uploading spots with `cwsweep-forward` to a DX-cluster node, an HTTP
  endpoint or a log file:

```sh
cwsweep-forward --from localhost:7300 --login N0CALL --cluster dxc.example.org:7300
cwsweep-forward --from localhost:7300 --login N0CALL --http https://example.org/api/spots --jsonl spots.jsonl
```

## Measured performance

`cwbench` runs the full engine on synthetic signals and scores the decoded
text against what was sent. SNR is key-down signal power relative to noise in
a 2500 Hz bandwidth (the WSJT-X convention). Unless stated otherwise: one
station, QSO or contest text, 18–32 WPM, AWGN, noise blanker on.

| SNR (2500 Hz) | −12 dB | −10 dB | −8 dB | −6 dB | −4 dB | 0 dB |
|---|---|---|---|---|---|---|
| character error rate | 87% | 42% | 8.8% | 0.7% | 0.2% | 0.0% |
| messages copied perfectly | 0% | 0% | 33% | 88% | 97% | 100% |
| false spots (60 trials) | 0 | 0 | 0 | 0 | 0 | 0 |

At 25 WPM, −8 dB in 2500 Hz is about +13 dB per-dit Eb/N0.

| Condition (40 trials) | −6 dB | 0 dB | +10 dB |
|---|---|---|---|
| random 5-character groups (no language-model help) | 0.8% | 0.0% | 0.0% |
| hand-sent, 15% timing jitter | 2.8% | 0.4% | 0.5% |
| heavy keying weight (+0.3 dit) | 1.7% | 0.0% | 0.0% |
| wide spacing: 5-dit letter gaps, 12-dit word gaps | 3.2% | 0.6% | 0.6% |
| Rayleigh QSB, 80% depth, 0.3 Hz | 64% | 11% | 0.0% |
| QRN, 20 impulses/s | 5.7% | 2.8% | 0.4% |
| 4 strong interfering stations | 1.4% | 0.0% | 2.1% |
| slow, 8–14 WPM | 0.1% | 0.3% | 0.4% |
| fast, 38–50 WPM | 6.2% | 0.0% | 0.0% |

Wideband: 30 s of 192 kHz I/Q with 40 stations decodes in about 5 s on one
core of a desktop CPU, about 6× real time. That run produced 30 spots, all
correct and none false. The stations not spotted had sent their call only
once.

Reproduce with, for example:

```sh
build/cwbench --snr -12:0:2 --trials 60
build/cwbench --snr -6,0,10 --qsb 0.8
build/cwbench --snr 0 --interferers 4 -v      # -v shows each miss: truth vs decoded
```

## Tools

* `cwgen` writes synthetic band recordings (audio or I/Q WAV) with any mix of
  stations, speeds, SNRs, fist jitter, QSB and QRN, for testing or for
  demonstrating cwsweep:
  ```sh
  build/cwgen --random 12 --snr -8:15 --qsb 0.3 --jitter 0.08 band.wav
  build/cwgen --fs 192000 --iq --random 40 --fmin -90000 --fmax 90000 wide.wav
  build/cwgen --station "700:25:-6:CQ CQ DE K1ABC K1ABC K" test.wav
  ```
* `cwbench` is the accuracy benchmark above. `--help` lists every scenario
  and tuning knob.
* `CWSWEEP_DEBUG=1` prints channel creation and every key-up/key-down run to
  stderr (`2` adds the space-progress events).

## Layout

```
src/core/   DSP and decoding library (no I/O)
  stft        channelizer, noise blanker
  keyer       Rician LLR, fading mixture, fixed-lag Viterbi keyer
  decoder     timing estimation, hard-decision beam decoder
  softdecoder explicit-duration HMM soft-decision decoder
  lm          character language model
  callsign    callsign syntax, spot policy, MASTER.SCP
  engine      channel detection and management, glue
  synth       synthetic signal generator
app/        cwsweep CLI: sources (sndfile/raw/PulseAudio/SoapySDR/openHPSDR),
            rigctld follower, telnet server; cwsend CLI
contrib/    cwsweep-forward spot relay, systemd units and example settings
docs/       daemon.md: running as a service, uploading spots
tools/      cwgen, cwbench
tests/      unit and end-to-end tests; stand-in radio and rigctld for testing
            the hpsdr source and --rigctl without hardware
```

## Limitations and ideas

Planned work is tracked in [TODOs.md](TODOs.md).

* Single-threaded. Channels are independent, so spreading them over cores is
  the obvious next step for very wide or very busy bands.
* No audio output or waterfall: this is a CLI decoder and spotter.
* The openHPSDR source has been tested against a protocol emulator
  (`tests/fake_hpsdr.py`), written from the protocol documents and the Hermes
  Lite 2 gateware, but not yet on a real radio. If the spectrum comes out
  mirrored, use `--swap-iq`. The same goes for `cwsend`: try it first into a
  dummy load at a low `--drive`.
* `cwsend` sends typed text only: no paddle or key input, no sidetone, and no
  filter-board (N2ADR, Alex) band switching. It can't share the radio with
  cwsweep, so there is no receiving while it runs.
* One receiver, one band per process. A Hermes Lite 2 can stream several
  receivers at once; using them for several bands is not implemented.
* An openHPSDR radio can't be shared with an SDR program (Thetis, SparkSDR,
  piHPSDR). Running alongside one needs its audio and `--rigctl`; taking I/Q
  from the program (TCI) is not implemented.
* Text tuning (thresholds, priors) was done mostly on synthetic signals,
  checked against a few off-air recordings. More real-band recordings would
  be the next calibration step: record with `--record` and replay.
* An operator whose letter gaps and word gaps overlap can't be split into
  words by timing alone; such text comes out with some words run together
  or some letters set apart.
* Stretched spacing is modelled, compressed spacing (letters run together) is
  not.
* A neural acoustic model (e.g. a small CNN/RNN producing per-frame mark
  probabilities) could replace the analytic LLR and feed the same soft
  decoder.
