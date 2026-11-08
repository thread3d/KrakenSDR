# 06 — RF verification with the antennas connected

Third session. Antennas are fitted (small ones). This records what is actually receivable and — more
importantly — the three receiver artefacts that will otherwise be mistaken for signals.

## Verdict

**The receive chain works end to end.** Antenna → KrakenSDR tuner → krakenrf `librtlsdr` →
`ogn-rf` → captured `.u8` file, all carrying real, strong RF.

Two tools do the measurement:

| Tool | Answers |
|---|---|
| `scripts/spectrum-probe.c` | is there RF at a given frequency right now (averaged + max-hold) |
| `scripts/analyze-u8.py` | what is in an `ogn-rf` raw capture, after the fact |

## What is actually receivable

### 868 MHz ISM band — strong

Probing 868.3 MHz for 5 s produced a pair of strong carriers:

```
868.0800 MHz   avg 30.0 dB   max-hold 56.8 dB
868.1185 MHz   avg 28.4 dB   max-hold 57.0 dB
868.0998 MHz   avg 19.6 dB   max-hold 46.6 dB
```

Two tones ~38 kHz apart around 868.10 MHz is the signature of a **2-FSK device on the 868.1 MHz SRD
channel** (868.0–868.6 MHz in Europe). Very loud — 57 dB above the noise floor.

### The same signals inside an ogn-rf capture

The decisive test: run `ogn-rf` itself, which captures 867.8–868.8 MHz at 1 MS/s, then analyse the
file it wrote. Same carriers, same frequencies:

```
continuous (averaged)      bursts (max-hold)
868.1186 MHz   26.4 dB     868.1188 MHz   51.0 dB
868.0803 MHz   26.2 dB     868.0800 MHz   50.6 dB
```

That closes the loop: the whole pipeline carries real RF, not noise.

### FM broadcast — receivable but weak

At 103.5 MHz, with the tuner deliberately offset 500 kHz below the station:

```
103.0000 MHz   avg 18.0 dB   max-hold 31 dB
```

The station is real and clean, but a *small* antenna is inefficient at 100 MHz (λ ≈ 3 m), so it only
shows up when you go looking for it properly.

### OGN / FLARM — not seen

Nothing at the two European OGN channels, **868.200** and **868.400** MHz. Expected: OGN carries
glider traffic, and there needs to be a transponder within range. This is not a hardware fault —
it is what an empty OGN band looks like.

## Three artefacts that look exactly like signals

This cost most of the session. All three are receiver behaviour, not RF.

### 1. The DC/LO spike at the tuned frequency

Every RTL-SDR has a spike at exactly the centre frequency, routinely **20–30 dB** above the noise
floor. Tuning to 103.0 MHz "found" a 30 dB carrier at 103.008 MHz. It was the spike. An FM carrier is
~200 kHz wide; the spike is a few kHz.

**Tell:** it sits exactly at the tuned frequency, always. **Fix:** exclude ±25 kHz, and if you must
measure a signal *at* the centre, retune so the signal is off-centre.

### 2. A band-edge artefact at −983 kHz

With a 2.048 Msps rate there is a fixed feature ~983 kHz below the tuned frequency — i.e. right at
the edge of the band. It appeared at 432.937, 867.317, 868.267 and 1089.017 MHz, and at first looked
like a signal present at every frequency.

**Tell — and this is the general test for any candidate:** tune away and watch what moves.

```
tuned 1090.0  ->  peak at 1089.0170 MHz
tuned 1090.5  ->  peak at 1089.5170 MHz
tuned 1091.0  ->  peak at 1090.0170 MHz
```

A real carrier keeps a fixed **absolute** frequency; this kept a fixed **offset**. Artefact.
**Fix:** mask the outer ~5% of the band.

### 3. Averaging hides bursty signals

OGN, ADS-B and most ISM devices transmit in short bursts. Averaging 64 frames divides a burst by ~64
and it vanishes into the noise. The 57 dB ISM carriers were invisible in an early averaged-only
measurement; they appeared as soon as a **max-hold** spectrum was kept.

**Fix:** always keep both, and capture for **seconds, not milliseconds**. A 128 ms capture has a poor
chance of containing an 850 ms OGN slot; the runs above used 5 s.

## Gain is working

Sanity check at 223 MHz, averaged noise floor versus requested gain:

| gain (dB) | noise floor |
|---|---|
| 0.0 | −13.89 dB |
| 7.7 | −13.77 dB |
| 19.7 | −12.70 dB |
| 32.8 | −3.19 dB |
| 42.1 | +3.95 dB |
| 49.6 | +10.34 dB |

Monotonic over ~24 dB. The tuner LNA is fine.

Ambient noise floor also varies with frequency (223 MHz ≈ +10 dB, 88 MHz ≈ −9 dB, 1296 MHz ≈ −13 dB)
which is the tuner's own frequency response plus the antenna's impedance match — not evidence of
signals on its own.

## Practical recipe

```bash
# Build
gcc -O2 -o spectrum-probe scripts/spectrum-probe.c -lSoapySDR -lfftw3f -lm

# Look at a band for 5 s, averaged + max-hold, DC and edges masked
./spectrum-probe 868.3 2.048 49.6 1000 25 5

# Any candidate? Retune and see whether it moves with the tuner.
./spectrum-probe 868.5 2.048 49.6 1000 25 5

# Then check an ogn-rf capture after the fact
./scripts/analyze-u8.py TEST0_2026.11.08.u8 -c 868.3 -r 1e6
```

## Note on the gain setting

The 57 dB ISM carriers were measured at **maximum gain (49.6 dB)**. That is far too hot for a 2.4 MHz
passband containing an 868.1 MHz transmitter a few metres away — it will desensitise the tuner and
the raised "grass" seen across the band is partly that. For OGN reception, drop to roughly 20–30 dB
and re-check. The `RF.OGN.Gain` value in the config is in tenths of dB (`400` = 40.0 dB); it should
come down.

## Next

1. Re-run the OGN capture at a sane gain and with a longer window, at 868.200 and 868.400.
2. Attach a decoder to `ogn-rf.fifo` and confirm slot handoff (still untested).
3. The multi-channel work: see [03-ogn-rf-integration.md](03-ogn-rf-integration.md).
