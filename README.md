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

- **The KrakenSDR is attached and its five receive chains are up.** All five `0bda:2838` RTL2838
  tuners (serials 1000–1004, R820T) enumerate behind the board's internal Microchip USB2517 hub.
  The RF path is confirmed live: output power scales with gain and the five channels differ from
  each other. **No real signal has been received yet** — there is no antenna connected and the
  antenna switch matrix is undriven, which is HeIMDALL's job.
- There is **no CP210x serial bridge**, and that is correct for this board: the noise source is
  switched over the RTL2832U GPIOs. See [docs/01-hardware.md](docs/01-hardware.md).
- `../KrakenSDR/krakensdr` was an empty directory; this repository now owns it.
- The host SDR stack is present but partial: SoapySDR 0.7.2 with an rtlsdr module, but the packaged
  `librtlsdr0 0.6.0-3` lacks `rtlsdr_set_dithering` and `rtlsdr_set_sample_freq_correction_f`, so
  **HeIMDALL cannot build against it** and needs the krakenrf fork. `librtlsdr-dev` and
  `libconfig-dev` are also missing, and `sudo` requires a password — so neither HeIMDALL nor
  `ogn-rf` can be built by `apt install` without help. See
  [docs/02-host-environment.md](docs/02-host-environment.md).

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
