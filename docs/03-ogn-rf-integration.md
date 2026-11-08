# 03 — Making ogn-rf work with the KrakenSDR

How `ogn-rf` consumes RF today, why that does not transplant onto a KrakenSDR, and the four
integration routes with a recommendation. The code changes live in the sibling
`../ogn-rf` checkout on branch **`krakensdr`**; this document is the design behind them.

## What ogn-rf is, and what it needs

`ogn-rf` is the OGN / FLARM RF front end. It is a single-threaded acquisition loop plus a
decoder that hands I/Q slots to `ogn-decode` over UDP. Everything RF-facing is one class:

```
rtlsdr.h   class RTLSDR      -- thin wrapper over librtlsdr
ogn-rf.cc  RF_Acq::Exec()    -- the acquisition loop, calls SDR.*
```

The acquisition loop's shape (verified in `ogn-rf.cc`, `RF_Acq::Exec`):

1. Align to the OGN time slot boundary: `OGN_StartTime = 0.375` s within each UTC second.
2. Read `OGN_SamplesPerRead = 850 ms × SampleRate` samples in one go.
3. Call `SDR.setCenterFreq()` for the *next* slot, then loop.

`SampleRate` defaults to **1 000 000** samples/s, 8-bit unsigned I/Q, and is configurable.

### The frequency plan decides everything

`freqplan.h` picks a plan from the station's latitude/longitude, and the four OGN/FLARM
time-slot frequencies are then derived per slot:

| Plan | Region | Base | Spacing | Channels | Span |
|---|---|---|---|---|---|
| 1 | Europe / Africa (default) | 868.200 MHz | 200 kHz | 2 | **0.2 MHz** |
| 2 | USA / Canada | 902.200 MHz | 400 kHz | 65 | **26.0 MHz** |
| 3 | Australia / S. America | 917.000 MHz | 400 kHz | 24 | **9.6 MHz** |
| 4 | New Zealand | 869.250 MHz | 200 kHz | 1 | 0 |

`calcCenterFreq()` collects the four `(slot, OGN)` frequencies and — crucially — tries to find a
single centre that covers **all of them** in one tuner pass:

```cpp
std::sort(HopFreq, HopFreq+4);
if((HopFreq[3]-HopFreq[0])<Band) CenterFreq=(HopFreq[0]+HopFreq[3]+1)>>1;
```

In **Europe** all four map onto just {868.2, 868.4} MHz, so the centre is a constant **868.3 MHz**
and ogn-rf in fact **never retunes**. That is the common case and by far the easiest target.

In the **USA** and **Australia** the channels are hash-hopped across 26 MHz / 9.6 MHz, far wider
than any single RTL-SDR passband, so ogn-rf genuinely retunes every slot.

> **This is the single most important fact for the integration, and it cuts both ways.**
> Where ogn-rf retunes, a KrakenSDR in coherent mode has to re-run its phase calibration after each
> retune (~1–3.5 s), which is longer than the 850 ms slot. Coherent DoA of OGN is therefore
> *incompatible* with hopping as written.

## The device-ownership problem

A KrakenSDR's five RTL2832U tuners share one clock and sit behind one internal hub. They are not
intended to be opened piecemeal by application code — **HeIMDALL** owns them, keeps them in lockstep
and publishes IQ frames. Two consequences:

- `ogn-rf`'s `librtlsdr` path cannot simply enumerate five dongles and use one; there is no
  coherent multi-dongle `rtlsdr_read_async`.
- Stock `librtlsdr` is also missing the entry points HeIMDALL itself needs. The installed library on
  the Xavier is `librtlsdr0 0.6.0-3`, and it does **not** export `rtlsdr_set_dithering` or
  `rtlsdr_set_sample_freq_correction_f`, both of which `heimdall_daq_fw/Firmware/_daq_core/rtl_daq.c`
  calls. HeIMDALL requires the **krakenrf fork**.

## Four routes

### A. HeIMDALL shared-memory consumer  *(recommended for coherent work)*

Attach to the `decimator_out_A/B` double buffer and read frames.

- Frames carry everything `ogn-rf` needs: `sampling_freq`, `rf_center_freq`, `cpi_length`,
  `time_stamp`, per-channel gains, and the sync/calibration flags.
- Channels are interleaved, so channel *k* of the array is a stride-`active_ant_chs` slice. This is
  what makes **OGN direction finding** possible at all: the same signal from five antennas.
- Cost: a shared-memory ABI that changes upstream, and a hard dependency on HeIMDALL running.

### B. HeIMDALL Ethernet IQ sink  *(recommended for a first cut)*

HeIMDALL can stream the identical frames over TCP (`_daq_core/iq_eth_sink.py`, port 5000).

- No ABI coupling at all: a socket read plus the published 1024-byte header layout.
- Lets `ogn-rf` run on a *different* machine from the radio, which is attractive here — the Jetson
  is the only box with the device, but nothing says the decoder must be.
- Slightly higher latency and a copy per frame; irrelevant at 1–2.4 MS/s.

### C. Independent mode — five frequencies, no hopping  *(the interesting one)*

In Coherent mode the array is one receiver. In **Independent** mode it is five receivers, each with
its own frequency and gain, and there is no calibration to redo.

- **Europe:** all OGN lives in 200 kHz at 868.3 MHz, so one tuner does everything and the other four
  are free for other bands.
- **USA:** 65 hash-hopped channels cannot be covered, but *the hopping stops mattering* — four
  tuners parked on the four current `(slot, OGN)` channels receive them **simultaneously** instead
  of taking turns, which removes ogn-rf's dead time rather than adding to it. Up to five.
- **No DoA** in this mode, and per the upstream suite no calibration runs at all.

### D. Direct librtlsdr with the krakenrf fork  *(simplest, least capable)*

Build the krakenrf `librtlsdr`, leave HeIMDALL out of the picture, and let `ogn-rf` open tuner *n*
exactly as it opens any dongle.

- Smallest code change: point `ogn-rf` at the fork and it works.
- No coherence, no DoA — you have an expensive single-channel OGN receiver.
- Collides with HeIMDALL if both run; two processes cannot own the same tuners.

## Recommendation

Two phases, deliberately ordered so the first is useful on its own:

**Phase 1 — make OGN reception work, without DoA.** Route **D** or **B** with a single channel at
868.3 MHz, fixed frequency (Plan 1 / NZ). This validates the whole chain — antenna, tuner, timing
slot alignment, decoder handoff — against a known-good OGN receiver, and it needs the fewest moving
parts. In Europe the plan is a fixed 868.3 MHz, so no retuning problem arises at all.

**Phase 2 — exploit the array.** Add a HeIMDALL shm/socket source (**A** or **B**) and run the
array in **Independent** mode (Plan 2/3) so the four hopping channels are received in parallel, or
in **Coherent** mode on a *fixed* channel where a static OGN/FLARM transmitter's bearing is
actually useful.

## Where the code goes

The clean seam is the `RTLSDR` class. `RF_Acq::Exec()` only ever calls this method surface:

```
isOpen()            getTime()          ResetBuffer()     Read(SampleBuffer<uint8_t>&, int)
Open(idx, freq, rate)   Close()        setCenterFreq(Hz) getFreqCorrection()
setTunerGainMode(m) setTunerGain(g)    setFreqCorrection(ppm)
setOffsetTuning(on) setBiasTee(on)     getDeviceIndexBySerial(serial)
```

None of that is intrinsically RTL-SDR-specific. So the branch introduces an alternative source class
with the **same method names and signatures**, selected by a new config key (proposed:
`RF.Source = "rtlsdr" | "heimdall"`), and `ogn-rf.cc` keeps calling `SDR.*` unchanged. That keeps the
diff small, keeps upstream merges cheap, and means the KrakenSDR path can be developed and tested
without forking the receiver logic.

```
ogn-rf (branch krakensdr)
├── rtlsdr.h                 unchanged -- the reference implementation of the interface
├── krakensdr_source.h       NEW  HeIMDALL frame source, same interface
├── krakensdr_source.cc      NEW  shm attach + double-buffer handshake + de-interleave
├── ogn-rf.cc                config selects the source class    (small diff)
└── docs/krakensdr.md        NEW  build + run notes
```

## Open questions

1. **Sample-rate match.** HeIMDALL's decimator output rate is set in `daq_chain_config.ini`;
   `ogn-rf` wants exactly 1 000 000 sps (or a configured value, and it derives FFT bin counts from
   it). Either configure the decimator to 1 MS/s or teach `ogn-rf` to resample. Configure the
   decimator — it is a config change versus a DSP change.
2. **Slot alignment.** ogn-rf aligns on 0.375 s using the host clock. HeIMDALL frames carry their
   own `time_stamp`; the right move is to align on that, not on `gettimeofday`, otherwise the two
   clocks drift apart within a slot.
3. **8-bit vs the decimator's output format.** `ogn-rf` is happiest with 8-bit unsigned I/Q (`u8`).
   Check what `data_type` / `sample_bit_depth` the decimator actually emits before writing the
   adapter — a float32 or int16 path needs a conversion step.
4. **Gain and calibration.** In coherent mode the noise-source calibration blanks reception for
   about a second. OGN slots are 850 ms. Do not calibrate during an acquisition slot; calibrate
   between passes or use Independent mode.
5. **Where does this leave the GSM frequency reference?** `ogn-rf` can lock its frequency against a
   nearby GSM base station (`GSM_CenterFreq`). That needs a second tuner and a retune away from
   868 MHz. On a five-channel radio that is cheap — one tuner can do GSM while another does OGN —
   but it must be coordinated with the mode choice above.
