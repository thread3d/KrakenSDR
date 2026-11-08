# 02 — Host environment (Jetson Xavier AGX)

What the Xavier gives us to build and run against. Everything below was measured on the device,
not assumed.

## Platform

| Item | Value |
|---|---|
| Hostname | `xavieragx` / `xavieragx.local` (`192.168.0.81`) |
| Kernel | `5.10.104-tegra`, `aarch64` |
| L4T | R35, revision 5.0 (JetPack 5.x line), dated 2024-02-20 |
| Distro | Ubuntu 20.04.6 LTS (Focal) |
| CPU/GPU | Tegra Xavier AGX (Carmel ARMv8 + Volta GPU) |
| Root filesystem | `/dev/nvme0n1p1`, 3.6 TB, ~304 GB used, ~3.2 TB free |
| Login user | `threaded` (uid 1000) |
| Access from dev box | `ssh -F /dev/null threaded@xavieragx.local` (see `../xsh`) |

The GPU is interesting: `ogn-rf` has an optional FFT path for the Raspberry Pi's Videocore
(`USE_RPI_GPU_FFT`, `gpu_fft.c` / `mailbox.c`). Those are VideoCore-specific and **will not** work
on a Tegra — the auto-detection in `Makefile` keys off `/opt/vc/src/hello_pi/hello_fft`, which is
absent, so the build correctly falls back to FFTW3 on the CPU. A CUDA/cuFFT path is a plausible
later optimisation, not a port.

## Toolchain

| Tool | Version |
|---|---|
| `gcc` / `g++` | 9.4.0 (Ubuntu 20.04.2) |
| `cmake` | 3.16.3 |
| `git` | 2.25.1 |
| `python3` | 3.8.10 |

`python3` at 3.8 is old. Upstream Kraken tools that use modern syntax, and the conda-based
HeIMDALL install, will be affected — note that HeIMDALL's own README targets 64-bit systems and
uses conda specifically for `numba` on ARM.

## SDR stack present

SoapySDR 0.7.2 is installed with a broad module set at
`/usr/lib/aarch64-linux-gnu/SoapySDR/modules0.7/`:

```
libHackRFSupport  0.3.3      libLMS7Support   20.01.0    libRedPitaya   0.1.1
libairspySupport  0.1.2      libaudioSupport  0.1.1     libbladeRF     0.4.1
libosmosdrSupport 0.2.5      libremoteSupport 0.5.1     librtlsdrSupport 0.3.0
libuhd
```

The `librtlsdr` **runtime** library is present — `/lib/aarch64-linux-gnu/librtlsdr.so.0` — and
`/etc/udev/rules.d/rtl-sdr.rules` is installed, which is what grants unprivileged access to RTL-SDR
dongles. `rtl_test` and friends are *not* installed (no `rtl-sdr` tools package).

## Missing, and why it matters

| Needed for | Package | State |
|---|---|---|
| `ogn-rf` (`rtl-sdr.h`) | `librtlsdr-dev` | **MISSING** — `/usr/include/rtl-sdr.h` absent |
| `ogn-rf` (`libconfig.h`) | `libconfig-dev` | **MISSING** — `/usr/include/libconfig.h` absent |
| `ogn-rf` (FFTW) | `libfftw3-dev` | installed — `/usr/include/fftw3.h` |
| `ogn-rf` (JPEG) | `libjpeg-turbo8-dev`, `libjpeg-dev` | installed — `/usr/include/jpeglib.h` |
| `scripts/iq-smoke-test.c` | `libsoapysdr-dev` | installed — the SoapySDR C API works |
| GitHub operations | `gh` CLI | **MISSING** |
| — | `rtl-sdr` tools | **MISSING** (`rtl_test` not found) |

Two of ogn-rf's four declared dependencies have no development headers on this host.

### The packaged librtlsdr cannot drive HeIMDALL

The installed runtime is `librtlsdr0 0.6.0-3` — the stock Ubuntu Focal build. HeIMDALL's
`Firmware/_daq_core/rtl_daq.c` calls two functions this build does not export:

```
$ nm -D --defined-only /lib/aarch64-linux-gnu/librtlsdr.so.0 | grep ...
rtlsdr_set_dithering                   absent
rtlsdr_set_sample_freq_correction_f    absent
rtlsdr_set_bias_tee_gpio               PRESENT
rtlsdr_set_tuner_bandwidth             PRESENT
```

So HeIMDALL **will not link** against the packaged library, regardless of `apt`. It needs the
[krakenrf fork](https://github.com/krakenrf/librtlsdr) built from source. That changes the calculus
below: a local-prefix source build is not merely a way around `sudo`, it is *required*.

### The `sudo` constraint

```
$ sudo -n true
sudo: a password is required
```

`threaded` is in the `sudo` group but passwordless sudo is not enabled, so **`apt install` cannot
be run non-interactively**. That rules out the ordinary fix. Three options, in order of
preference:

1. **Ask for the password once** and `sudo apt install librtlsdr-dev libconfig-dev` — smallest,
   most maintainable, and puts the headers where every future build expects them.
2. **Build the two dependencies from source into a local prefix** inside this project
   (e.g. `../ogn-rf/deps/prefix`) and point the build at it with `CFLAGS`/`LDFLAGS` or
   `PKG_CONFIG_PATH`. No root needed, fully reproducible, and it lets us use the *KrakenRF* fork of
   librtlsdr — which is arguably what we want anyway, since it carries the coherent multi-dongle
   support. Costs a little build time and a `-Wl,-rpath` to get right.
3. **Do nothing and treat ogn-rf as unbuilt** — not useful, so not really an option.

`scripts/check-env.sh` reports all of the above so the state is always observable rather than
remembered.

### Note on which librtlsdr

There are two relevant forks and they are not interchangeable by accident:

- **glidernet** (what `ogn-rf` documents as a requirement) — adds the **bias-T** control that OGN
  receivers use to power an active antenna.
- **krakenrf** — adds the **coherent multi-dongle** support KrakenSDR needs.

For classic single-dongle OGN reception on the Xavier, the glidernet fork is the correct target.
For the KrakenSDR path the devices are owned by HeIMDALL anyway, so `ogn-rf` will not link against
`librtlsdr` at all in that configuration — it will consume HeIMDALL frames instead. See
[03-ogn-rf-integration.md](03-ogn-rf-integration.md).

## Credentials and remote access

- The Xavier has **no** `gh`, **no** `~/.gitconfig` (no commit identity), and **no** working
  GitHub SSH key (`git@github.com: Permission denied (publickey)`).
- It *does* have unrestricted outbound network: DNS resolves, TCP 443 connects, and
  `git clone https://github.com/...` works.
- Therefore pushes to GitHub are performed **from the development box**, which has an
  authenticated `gh` (account `thread3d`, `repo` scope), over an SSH clone of this repository.

## What this means for the plan

The Xavier is a capable host — 3.2 TB free, a modern ARM core, a GPU, and a real SDR stack. The
gaps are all *packaging* gaps (dev headers, `gh`, `sudo`), not capability gaps. None of them block
the repository or the integration design; they block only the final `make` of ogn-rf.
