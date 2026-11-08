# 01 — KrakenSDR hardware

What the device is, how it shows up on a Linux USB bus, and what that means for software on the
Jetson. This page separates **verified** facts from **upstream-documented** expectations and from
**unconfirmed** assumptions, because at the time of writing no KrakenSDR has enumerated on the
Xavier.

## What it is

A KrakenSDR is a five-channel coherent software-defined radio built from RTL-SDR silicon:

- **5 × RTL2832U + R820T2** receive chains, sharing one clock so that their samples stay
  phase-coherent with each other.
- An internal **switching matrix** and a **calibration noise source**, used to measure and correct
  the phase and amplitude differences between channels.
- A single **USB** connection to the host, behind an internal USB hub, alongside a **USB-serial
  bridge** used for control (noise source, switching).
- Tuning range is the R820T2 range, roughly **24 MHz – 1.766 GHz**, with about **2.4 MHz** of
  instantaneous bandwidth per channel.

> *Verified:* the five-channel coherent architecture, the noise-source calibration, the
> Coherent / Wideband / Independent operating modes, and that HeIMDALL is the acquisition layer.
> These are described in `krakenrf/krakensdr_suite`'s README and `krakenrf/heimdall_daq_fw`'s README.

> *Upstream-documented, not yet confirmed on this unit:* the exact USB vendor/product IDs. The
> expected signature is several `0bda:2838` (Realtek RTL2838 DVB-T) interfaces plus a
> `10c4:ea60`-family (`cp210x`) serial bridge. `scripts/check-device.sh` reports whatever is
> actually there rather than asserting these.

## The important architectural point

**A KrakenSDR does not appear to an application as five normal RTL-SDR dongles you can `open()`.**

Because the five receivers share one clock and must be sampled in lockstep, the vendor design puts
the USB devices under the control of a single acquisition daemon — **HeIMDALL** — which:

1. configures and starts all five tuners together,
2. runs the delay/phase synchroniser and the calibration pass,
3. publishes a stream of **IQ frames** into POSIX shared memory (or over a TCP socket).

Application software (the direction-finding suite, GNU Radio flowgraphs, anything custom) is a
*consumer* of that stream, not a driver of the hardware.

This matters directly for the OGN work: `ogn-rf` is written against `librtlsdr` and calls
`rtlsdr_read_async()` on one dongle. It cannot simply be pointed at a KrakenSDR. See
[03-ogn-rf-integration.md](03-ogn-rf-integration.md).

## The IQ frame interface

HeIMDALL's frame format is defined in
`heimdall_daq_fw/Firmware/_daq_core/iq_header.h`. Each frame is a **1024-byte header** followed by
the sample payload:

| Field | Meaning |
|---|---|
| `sync_word` | `0x2bf7b95a` — frame valid marker |
| `frame_type` | `DATA` / `DUMMY` / `RAMP` / `CAL` / `TRIGW` |
| `hardware_id`, `unit_id` | board identification |
| `active_ant_chs` | how many channels are interleaved in the payload |
| `rf_center_freq`, `adc_sampling_freq`, `sampling_freq` | tuning and rate, in Hz |
| `cpi_length` | samples per frame |
| `time_stamp`, `daq_block_index`, `cpi_index` | position in the stream |
| `data_type`, `sample_bit_depth` | payload encoding |
| `if_gains[32]` | per-channel IF gain |
| `delay_sync_flag`, `iq_sync_flag`, `sync_state` | coherence/calibration status |
| `noise_source_state` | calibration noise source on/off |

Buffers are **double-buffered** (`..._A` / `..._B`) and hand-shaken with a pair of control FIFOs
(`fw_` = consumer→producer, `bw_` = producer→consumer). The stage names a consumer may attach to
are enumerated in `sh_mem_util.h`:

```
HEIMDALL_DAQ_FW_GEN_STD_FRAME_{A,B}   raw generated frames
decimator_in_{A,B}                    pre-decimation
decimator_out_{A,B}                   post-decimation  <-- what DSP clients normally read
delay_sync_iq_{A,B}                   delay-synchronised IQ
```

A consumer attaches with `init_in_sm_buffer()`, then loops: `wait_buff_ready()` → process →
`send_ctr_buff_free()`. There is also an **Ethernet IQ sink**
(`Firmware/_daq_core/iq_eth_sink.py`, TCP port 5000) which streams the same frames over a socket —
a far lower-coupling integration path, discussed in the integration doc.

## Expected USB signature

When the device is working, `lsusb` should show something like:

```
Bus 00X Device 00Y: ID 0bda:2838 Realtek Semiconductor Corp. RTL2838 DVB-T    (×5)
Bus 00X Device 00Z: ID 10c4:ea60 Silicon Labs CP210x UART Bridge              (×1)
```

and `/dev/ttyUSB0` (the CP210x bridge) should appear, plus five RTL2832U entries under
`/sys/bus/usb/devices/`. Note the **power** requirement: the KrakenSDR is USB-bus-powered but draws
enough current that a marginal hub or cable shows up as devices that enumerate and then vanish, or
that never enumerate at all. That failure mode looks exactly like "no cable attached".

## Observed state on the Xavier — CONFIRMED PRESENT

Verified 2026-11-08. All five receive chains enumerate behind the board's internal hub:

```
1-4.3      0424:2517  Microchip USB2517 Hub      <- internal hub
1-4.3.2    0bda:2838  RTL2838UHIDIR  serial=1003   R820T
1-4.3.3    0bda:2838  RTL2838UHIDIR  serial=1002   R820T
1-4.3.4    0bda:2838  RTL2838UHIDIR  serial=1001   R820T
1-4.3.5    0bda:2838  RTL2838UHIDIR  serial=1000   R820T
1-4.3.6    0bda:2838  RTL2838UHIDIR  serial=1004   R820T
```

and the receive path is live — power rises with gain and the channels differ from each other.
See [04-status-log.md](04-status-log.md) for the measurements.

The VID:PID assumption above was right (`0bda:2838` ×5), but the **serial bridge is genuinely
absent**: there is no CP210x and no `/dev/ttyUSB*`. That is correct for this board, which drives the
noise source and switch matrix over the RTL2832U GPIOs instead. Do not treat a missing `ttyUSB` node
as a fault.

> **Trap: enumeration order ≠ channel order.** SoapySDR/librtlsdr list the tuners as
> `1004, 1000, 1001, 1002, 1003` for `rtl = 0..4`. Always address a tuner by its EEPROM serial.

## Troubleshooting checklist


Work down this list in order; each step has a script.

1. **Physical** — known-good USB 3 cable, and a port that can supply the current. Prefer a
   powered hub or a direct port on the Xavier, not a bus-powered hub behind another hub. The
   Xavier's bus already has two nested hubs in the chain, which is a plausible culprit.
2. **Enumeration** — `scripts/check-device.sh`. Look for the CP210x bridge: it is simpler and
   lower-power than the five tuners, so it usually appears even when the tuners fail.
3. **Kernel log** — `sudo dmesg -w` while plugging in. Look for `usb X-Y: new full-speed USB
   device`, `cp210x`, `usb-storage` or repeated `device descriptor read/64, error -71` (a signalling
   or cable fault) / `-110` (timeout) / `over-current`.
4. **Power** — if the device enumerates then drops, try a powered hub. `dmesg` will say
   `over-current change` or `unable to enumerate USB device`.
5. **Permissions** — once it appears, the serial bridge needs group access. `threaded` is already
   in `plugdev`, `dialout` is *not* in `groups` on this host; the `rtl-sdr.rules` udev file is
   installed at `/etc/udev/rules.d/rtl-sdr.rules`.

## Notes that will matter later

- Only **~2.4 MHz** of bandwidth per channel, and the R820T2 has a real high-pass response that
  rolls off below ~24 MHz. The OGN / FLARM band at **868.3 MHz** and the 1090 MHz ADS-B band are
  both comfortably in range.
- The **noise source** is switched into the antenna path for calibration. It must be off while
  receiving real signals, and any calibration pass blanks reception for roughly a second.
- Coherent mode has to be **recalibrated after every retune** — the upstream suite quotes ~3.5 s
  for this on a Raspberry Pi. On a Jetson Xavier AGX this should be faster, but it is the single
  biggest obstacle to hopping around the band, and therefore to multi-channel OGN scanning.
