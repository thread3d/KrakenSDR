# 04 — Status log

Dated record of what was tried and what it produced. Newest last. Measurements are quoted from
actual command output, not recalled.

---

## 2026-11-08 — Session 1: bring-up and repository setup

### Host access

Reached `xavieragx.local` (`192.168.0.81`) over SSH as user `threaded`. The system SSH config on
the development box is broken by ownership, so access goes through the `../xsh` wrapper
(`ssh -F /dev/null`, no host-key checking). Verified:

```
xavieragx / threaded
Linux xavieragx 5.10.104-tegra #1 SMP PREEMPT ... aarch64
Ubuntu 20.04.6 LTS, L4T R35 rev 5.0
```

### The KrakenSDR folder was empty

`~/projects/KrakenSDR/` contained exactly one empty subdirectory, `krakensdr/`, 8 KB of directory
nodes and no files. This repository now owns that path.

### The device was absent, then appeared

First scan showed **no** SDR hardware at all — 8 USB devices, all hubs, a keyboard and a mouse, and
no `/dev/ttyACM*`, `/dev/ttyUSB*` or `/dev/serial/by-id/`. The user re-seated the cable.

A 60-second `udevadm monitor` caught the enumeration, and the bus went from 8 to 14 devices:

```
1-4.3      0424:2517  Microchip USB2517 Hub          <- the KrakenSDR's internal hub
1-4.3.2    0bda:2838  RTL2838UHIDIR  serial=1003
1-4.3.3    0bda:2838  RTL2838UHIDIR  serial=1002
1-4.3.4    0bda:2838  RTL2838UHIDIR  serial=1001
1-4.3.5    0bda:2838  RTL2838UHIDIR  serial=1000
1-4.3.6    0bda:2838  RTL2838UHIDIR  serial=1004
```

**All five receive chains present.** Each reports a `Rafael Micro R820T` tuner via SoapySDR.

Two things worth recording:

- **There is no CP210x serial bridge and no `/dev/ttyUSB*`.** This is *expected*, not a fault: the
  calibration noise source and switch matrix are driven over the RTL2832U's GPIOs
  (`rtlsdr_set_bias_tee_gpio` in `rtl_daq.c`), not over a serial port. Do not chase a missing
  `ttyUSB` as a bug.
- **SoapySDR's enumeration order is not the channel order.** It lists serials
  `1004, 1000, 1001, 1002, 1003` as `rtl = 0..4`. Any code that assumes index 0 is channel 0 is
  wrong. Address tuners by EEPROM serial (1000–1004), which is what `scripts/iq-smoke-test.c` does.

### Receive path verified

`scripts/iq-smoke-test.c` (SoapySDR C API, `libsoapysdr-dev` is installed) opens all five by serial,
tunes, reads 131 072 IQ pairs and reports power.

At 868.3 MHz / 1.024 Msps / **gain 0 dB** every channel sits on the quantisation floor:
peak ≈ 2, `|x| ≤ 1` for 99.9 % of samples.

At **gain 49.6 dB** (the tuner maximum) the picture changes and the channels separate:

| ch | serial | mean power | peak | `|x| ≤ 1` |
|---|---|---|---|---|
| 0 | 1000 | 3.6 | 8 | 57.4 % |
| 1 | 1001 | 2.2 | 7 | 69.0 % |
| 2 | 1002 | 1.2 | 5 | 83.6 % |
| 3 | 1003 | 1.3 | 5 | 82.0 % |
| 4 | 1004 | 2.8 | 8 | 64.8 % |

**Conclusion: the RF path is live and gain control works.** Power tracks gain, and the five
channels differ from each other exactly as five independent receivers should. The absolute levels
are low (peak 5–8 of 127) because there is no antenna on the ports and the KrakenSDR's antenna
switch matrix is undriven — HeIMDALL is what normally routes signals and sets the matrix up. That is
consistent, not a fault, but **no real signal has been received yet**.

Note `[R82XX] PLL not locked!` is printed on every tune by the stock driver. It did not prevent
gain-dependent output, but it is worth re-testing once the krakenrf librtlsdr fork is in use, to see
whether it disappears.

### The stock driver is not enough for HeIMDALL

Installed library is `librtlsdr0 0.6.0-3` (Ubuntu Focal). HeIMDALL's `rtl_daq.c` calls two
functions this library does not export:

```
rtlsdr_set_dithering                   absent
rtlsdr_set_sample_freq_correction_f    absent
```

So **HeIMDALL cannot be built against the packaged librtlsdr.** It needs the
[krakenrf fork](https://github.com/krakenrf/librtlsdr). Similarly `ogn-rf` needs
`librtlsdr-dev` and `libconfig-dev`, and **neither development header is installed**.

### Blocked on packaging, not capability

`sudo -n true` fails — passwordless sudo is not enabled, so `apt install` cannot run
unattended. `threaded` *is* in the `sudo` and `plugdev` groups.

Consequence: to build anything (HeIMDALL, ogn-rf) we need either the sudo password once, or a
local-prefix source build. The second is attractive anyway because the krakenrf librtlsdr fork is
required regardless — see [02-host-environment.md](02-host-environment.md).

### ogn-rf surveyed

Cloned `thread3d/ogn-rf` (single `master` branch) and `krakenrf/heimdall_daq_fw` and
`krakenrf/krakensdr_suite` for reference. Design conclusions are in
[03-ogn-rf-integration.md](03-ogn-rf-integration.md); the headline finding is that in **Europe**
ogn-rf never retunes (both OGN channels fit one passband at 868.3 MHz), which makes the coherent-DoA
calibration problem tractable, while in the **USA/Australia** it hash-hops over 26 / 9.6 MHz, which
makes coherent mode impractical but makes *Independent mode across five tuners* a genuinely better
receiver than the single-dongle original.

### Next actions

1. Create the private GitHub repo and push this tree. *(done — see the repository itself)*
2. Clone ogn-rf to `~/projects/KrakenSDR/ogn-rf` with branch `krakensdr`.
3. Get the sudo password, **or** build the krakenrf `librtlsdr` fork plus `libconfig` into a local
   prefix, then build `ogn-rf` and confirm it compiles on `aarch64`.
4. Connect an antenna and re-run the smoke test to confirm real signal — a 1090 MHz ADS-B or 868 MHz
   ISM band is the easiest thing to see.
5. Evaluate running HeIMDALL on the Xavier: its documented target is a Pi 4, and the Xavier is far
   stronger, but Python 3.8 and the conda-based install are both friction points.
