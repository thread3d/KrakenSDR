# KrakenSDR on the NVIDIA Jetson Xavier AGX

Working repository for bringing a **KrakenSDR** (5-channel coherent RTL-SDR array) up on an
NVIDIA **Jetson Xavier AGX**, and for making it feed the
[OGN](http://glidernet.org) RF receiver [`thread3d/ogn-rf`](https://github.com/thread3d/ogn-rf).

This repository holds the scripts, code and documentation produced for that work. It is *not* a
fork of any upstream KrakenSDR component — those live upstream (see
[docs/03-ogn-rf-integration.md](docs/03-ogn-rf-integration.md)) and in the sibling
`../ogn-rf` checkout, which carries the integration work on its `krakensdr` branch.

---

## The two machines

| Role | Machine | Notes |
|---|---|---|
| Host under test | `xavieragx.local` (`192.168.0.81`) | Jetson Xavier AGX, Ubuntu 20.04.6 LTS, L4T R35.5.0, `aarch64` |
| Development / control | `MacPro71Fedora` (this box) | Reaches the Jetson over SSH as user `threaded` |

Access is via the `xsh` helper in the parent directory, which bypasses a broken system SSH config:

```bash
../xsh 'lsusb'          # run a command on the Jetson
```

## Repository layout

```
krakensdr/
├── README.md                  this file
├── docs/
│   ├── 01-hardware.md         KrakenSDR architecture and USB signatures
│   ├── 02-host-environment.md what the Xavier's software stack provides and lacks
│   ├── 03-ogn-rf-integration.md  how ogn-rf could consume the KrakenSDR
│   ├── 04-status-log.md       dated log of what has been tried and learned
│   ├── 05-fftw-float-planning.md the startup stall, root-caused
│   └── 06-rf-verification.md  what is receivable, and what only looks receivable
└── scripts/
    ├── check-device.sh        is a KrakenSDR actually attached, and how
    ├── check-env.sh           dump the toolchain / SDR stack / build deps
    ├── watch-usb.sh           watch for the device enumerating
    ├── build-deps.sh          build librtlsdr fork + libconfig, no root
    ├── iq-smoke-test.c        do all five tuners actually produce IQ
    ├── spectrum-probe.c       averaged + max-hold spectrum, DC/edge masked
    ├── analyze-u8.py          find signals in an ogn-rf .u8 capture
    ├── multi-tuner-test.c     stream 1-5 tuners concurrently, one thread each
    └── fftw-plan-timing.c     isolate FFTW planning cost
```

## Current state (see [docs/04-status-log.md](docs/04-status-log.md) for the history)

- **The KrakenSDR is attached and working.** All five `0bda:2838` RTL2838 tuners (serials 1000–1004,
  R820T) enumerate behind the board's internal Microchip USB2517 hub, and the RF path is live.
- **`ogn-rf` now receives real RF on it.** Branch `krakensdr` of
  [`thread3d/ogn-rf`](https://github.com/thread3d/ogn-rf) builds on `aarch64`, opens a tuner at
  868.300 MHz / 1.000 Msps, and the captures contain genuine signals: strong 868 MHz ISM carriers at
  868.080 / 868.119 MHz at **57 dB** above the noise floor, confirmed both by direct spectrum
  measurement *and* inside an `ogn-rf` `.u8` capture. See
  [docs/06-rf-verification.md](docs/06-rf-verification.md).
- **No OGN signal yet.** Nothing at the European OGN channels (868.200 / 868.400 MHz) — expected,
  since OGN carries glider traffic and needs a transponder in range. No decoder is attached to
  `ogn-rf.fifo` yet either, so slot handoff is the one untested link.
- Getting there needed one source change: `fft.h` hard-coded `FFTW_MEASURE`, and single-precision
  FFTW planning on this host takes **37 s for 4096 points and 61 s for 32768** (double precision:
  20–40 ms). `ogn-rf` plans three float transforms in global constructors, so it stalled with no
  output at all for minutes before `main()`. See
  [docs/05-fftw-float-planning.md](docs/05-fftw-float-planning.md).
- Dependencies (`librtlsdr` from the krakenrf fork, and `libconfig`) are built into `deps/prefix`
  with no root by `scripts/build-deps.sh`; the packaged `librtlsdr0 0.6.0` is unusable for HeIMDALL.
  See [docs/02-host-environment.md](docs/02-host-environment.md).
- There is **no CP210x serial bridge**, and that is correct for this board: the noise source is
  switched over the RTL2832U GPIOs. See [docs/01-hardware.md](docs/01-hardware.md).

## Quick start

```bash
# 1. Is the hardware there?
./scripts/check-device.sh

# 2. If not, plug it in and watch for it
./scripts/watch-usb.sh 120

# 3. What does this host give us to build against?
./scripts/check-env.sh
```

All scripts are POSIX `sh`/`bash`, dependency-free, and safe to re-run. They exit non-zero when
they find a problem so they can be used in CI or from a provisioning step.

## Upstream references

KrakenRF publishes the receiver stack across several repositories:

| Repository | Role |
|---|---|
| [`krakenrf/heimdall_daq_fw`](https://github.com/krakenrf/heimdall_daq_fw) | HeIMDALL DAQ firmware — coherent acquisition chain; owns the USB devices and publishes IQ frames |
| [`krakenrf/krakensdr_suite`](https://github.com/krakenrf/krakensdr_suite) | KrakenSDR V2 suite — MUSIC DoA, beamforming, scanners, decoders (all C++) |
| [`krakenrf/krakensdr_doa`](https://github.com/krakenrf/krakensdr_doa) | Previous-generation DoA application (Python) |
| [`krakenrf/librtlsdr`](https://github.com/krakenrf/librtlsdr) | librtlsdr fork with the coherent multi-dongle support KrakenSDR needs |
| [`krakenrf/krakensdr_docs`](https://github.com/krakenrf/krakensdr_docs) | Wiki and hardware documentation |

## Licence

The material here is original work and is provided as-is for use with the KrakenSDR and OGN
projects. Upstream components keep their own licences — `ogn-rf` and HeIMDALL are **GPL v3**.
Keep that in mind before combining code from the two trees.
