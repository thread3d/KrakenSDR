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
│   └── 04-status-log.md       dated log of what has been tried and learned
└── scripts/
    ├── check-device.sh        is a KrakenSDR actually attached, and how
    ├── check-env.sh           dump the toolchain / SDR stack / missing build deps
    └── watch-usb.sh           watch for the device enumerating
```

## Current state (see [docs/04-status-log.md](docs/04-status-log.md) for the history)

- **The KrakenSDR is attached and working.** All five `0bda:2838` RTL2838 tuners (serials 1000–1004,
  R820T) enumerate behind the board's internal Microchip USB2517 hub, and the RF path is live.
- **`ogn-rf` now receives on it.** Branch `krakensdr` of
  [`thread3d/ogn-rf`](https://github.com/thread3d/ogn-rf) builds on `aarch64`, opens a tuner at
  868.300 MHz / 1.000 Msps and acquires valid 8-bit IQ (5,099,640 bytes in 10 s, mean 127.37 about
  the 128 centre, stddev 9.91). **No OGN signal has been decoded yet** — only IQ acquired — and no
  antenna is connected.
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
