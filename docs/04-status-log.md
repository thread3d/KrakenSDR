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

---

## 2026-11-08 — Session 1b: dependencies, ogn-rf build, and the startup stall

### Dependencies built without root

`sudo` needs a password, so `apt install librtlsdr-dev libconfig-dev` was not an option. Worse, the
packaged `librtlsdr0 0.6.0` lacks the two fork-only symbols HeIMDALL needs, so the krakenrf fork was
required anyway.

`scripts/build-deps.sh` cloned and installed both into `deps/prefix` with no root:

```
librtlsdr (krakenrf fork) : built and installed
libconfig                 : built and installed (cmake)
symbol rtlsdr_set_dithering                : present
symbol rtlsdr_set_sample_freq_correction_f : present
```

That removes the packaging blocker entirely. `sudo` is still the cleaner long-term answer for
`libjpeg`/`fftw` but is no longer required.

### ogn-rf builds and runs on aarch64

`ogn-rf` compiled cleanly against the local prefix (warnings only, all pre-existing) and links the
krakenrf fork via rpath. A trivial program confirmed the fork sees all five tuners:

```
TRIVIAL: rtlsdr_get_device_count = 5
```

### The "hang" that was actually FFTW planning

Running `ogn-rf` looked like a hard deadlock: **100 % of one core, single-threaded, in user space,
no output at all**, no USB device opened, acquisition thread never created — for over three minutes,
with `utime` tracking wall time one-for-one.

It was not a deadlock. `fft.h` requested `FFTW_MEASURE`, and on this host **single-precision** FFTW
planning is pathologically slow:

| transform | points | `FFTW_MEASURE` plan time |
|---|---|---|
| float (`fftwf`) | 4 096 | **37.144 s** |
| float (`fftwf`) | 32 768 | **60.858 s** |
| double (`fftw`) | 4 096 | 0.022 s |
| double (`fftw`) | 32 768 | 0.038 s |

A ~1 700× gap between two transforms of the same size in the same library. `ogn-rf` plans three
float transforms **in global constructors, before `main()`**, so it spends well over 100 s inside
`fftwf_plan_dft_1d` before printing anything. Because nothing is logged, it reads as a crash.

Full analysis, reproduction and four candidate fixes: [05-fftw-float-planning.md](05-fftw-float-planning.md).
Reproduce with `scripts/fftw-plan-timing.c`.

### Result: OGN reception works on the KrakenSDR

With `FFTW_ESTIMATE` (now the branch default, overridable via `-DFFTW_PLAN_FLAG`), `ogn-rf` opens a
tuner immediately and acquires real OGN slots:

```
RTLSDR::Open(1,868300000,1000000) => Generic RTL2832U OEM,  868.300 MHz, 1.000 Msps
HTTP_Server.Exec() ... Listening on port 8080
```

Capture verified — 10 s of acquisition produced `TEST0_2026.11.08.u8`:

```
bytes            5,099,640
min/max          0 / 255
mean             127.37   (128 = zero I/Q, so no DC offset)
stddev           9.91
distinct values  256
>3 from centre   63.2 %
```

That is a healthy Gaussian IQ distribution — live noise and signal, not zeros and not a stuck ADC.

Note it opened **device index 1** for serial `1000`, confirming the enumeration-order trap
documented in [01-hardware.md](01-hardware.md).

The `Half time slot` / `Dropped a slot` messages in that run are expected: nothing was draining
`ogn-rf`'s output FIFO, so `OutQueue` backed up. They are not a defect.

### Two runtime traps worth recording

Both made a working program look broken:

1. **stdin at EOF kills it.** `main()` loops on `fgets(stdin)`; on EOF it breaks, sleeps 4 s and
   exits having done nothing. Under `nohup`, a script, or a service manager this is exactly what
   happens. It needs `sleep infinity | ogn-rf ...` or a pty.
2. **Block-buffered stdout hides everything.** `printf` to a pipe is buffered, so `timeout`-killing
   the process discards its output entirely. Use `stdbuf -oL -eL` or a file.

Also: **ptrace is blocked** on this host (`/proc/sys/kernel/yama/ptrace_scope`), so `gdb -p` cannot
attach and stack-trace a stuck process. Working around that cost real time; the instrumented-copy
technique (insert `fprintf(stderr, ...)` at known anchors, rebuild in `/tmp`) is the practical
substitute.

### Committed

Branch `krakensdr` on `thread3d/ogn-rf`, commit `f0a752c`:

- `fft.h` — `FFTW_MEASURE` → overridable `FFTW_PLAN_FLAG`, defaulting to `FFTW_ESTIMATE`
- `docs/krakensdr.md` — verified config, build and run commands, the FFTW analysis, the two traps

### Next actions

1. **FFTW wisdom caching** for `ogn-rf`: keep the (faster) measured plans but persist them, so the
   cost is paid once instead of every start. This is the proper fix behind the `FFTW_PLAN_FLAG` stopgap.
2. **Move FFT plan creation out of global constructors** into `main()` after configuration, so a slow
   plan is at least visible in the logs instead of silent.
3. **Attach a decoder** (`ogn-decode`, or any consumer of `ogn-rf.fifo`) and confirm end-to-end slot
   handoff, which is the first test that exercises the 0.375 s slot alignment properly.
4. **Connect an antenna.** No real OGN signal has been decoded yet — only IQ acquired.
5. Then the multi-channel work: the HeIMDALL source adapter and, for USA/Australia, Independent mode
   across the five tuners (see [03-ogn-rf-integration.md](03-ogn-rf-integration.md)).
6. Consider building FFTW from source for `aarch64` with NEON enabled — a 1 700× float/double
   planning gap suggests the system `libfftw3f3` is badly built, and runtime transforms may be
   suffering the same way.


---

## 2026-11-08 — Session 1c: antennas connected, RF verified

Antennas are now fitted (small ones). Re-measured everything with `scripts/spectrum-probe.c`, a new
tool that keeps **both** an averaged and a **max-hold** spectrum and masks the DC spike and the band
edges.

### The chain works end to end

Strong carriers found in the 868 MHz ISM band at 868.3 MHz (5 s capture, max gain):

```
868.0800 MHz   avg 30.0 dB   max-hold 56.8 dB
868.1185 MHz   avg 28.4 dB   max-hold 57.0 dB
```

Two tones ~38 kHz apart around 868.10 MHz — the signature of a 2-FSK device on the 868.1 MHz SRD
channel.

Then the decisive test: run `ogn-rf` itself and analyse the `.u8` file it wrote
(`scripts/analyze-u8.py`). Same carriers, same frequencies:

```
868.1186 MHz   26.4 dB avg   51.0 dB max-hold
868.0803 MHz   26.2 dB avg   50.6 dB max-hold
```

Antenna → tuner → krakenrf librtlsdr → ogn-rf → capture file, all carrying real RF.

FM broadcast is receivable too — a clean 31 dB carrier at 103.000 MHz — but only if you go looking
for it properly, because a small antenna is very inefficient at 100 MHz.

### No OGN

Nothing at 868.200 or 868.400 MHz, the two European OGN channels. That is expected: OGN carries
glider traffic and there needs to be a transponder in range. Not a hardware fault.

### Three artefacts that cost most of the session

All three present as convincing "signals":

1. **DC/LO spike at the tuned frequency**, 20–30 dB above the floor. "Found" a 30 dB carrier at
   103.008 MHz that was nothing. Exclude ±25 kHz.
2. **Band-edge artefact at −983 kHz** (at 2.048 Msps). It appeared at 432.937, 867.317, 868.267 and
   1089.017 MHz and looked like a signal present everywhere. The test that settles it: retune.
   Tuning 1090.0 → 1090.5 → 1091.0 moved it 1089.017 → 1089.517 → 1090.017, i.e. it kept a fixed
   **offset**, not a fixed **frequency**. An artefact. **This retune test is the general way to
   tell a real carrier from a receiver artefact.** Mask the outer 5%.
3. **Averaging hides bursts.** OGN, ADS-B and ISM traffic is bursty; averaging 64 frames divides a
   burst by 64. The 57 dB ISM carriers were invisible in an averaged-only measurement. Keep a
   max-hold spectrum and capture for **seconds**, not 128 ms.

### Gain sanity check

Noise floor versus requested gain at 223 MHz: 0 dB → −13.89, 19.7 → −12.70, 32.8 → −3.19,
42.1 → +3.95, 49.6 → +10.34. Monotonic over ~24 dB; the LNA is fine.

### Important consequence for the OGN config

The strong ISM carriers were measured at **maximum gain**. That is far too hot — with an 868.1 MHz
transmitter nearby it desensitises the tuner, and the ~10 dB "grass" seen across the band is partly
that. `RF.OGN.Gain` is in tenths of dB, so the `400` (40.0 dB) currently in the test config should
come down to roughly 200–300 before judging OGN reception.

### Added

- `scripts/spectrum-probe.c` — averaged + max-hold spectrum with DC and edge masking
- `scripts/analyze-u8.py` — the same analysis over an `ogn-rf` capture
- `docs/06-rf-verification.md` — the full write-up, including the retune test

### Next actions

1. Re-run the OGN capture at 200–300 (20–30 dB) gain, 868.200 and 868.400, for several minutes.
2. Attach a consumer to `ogn-rf.fifo` and confirm slot handoff — still the untested link.
3. Multi-channel work per [03-ogn-rf-integration.md](03-ogn-rf-integration.md).
