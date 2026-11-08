#!/usr/bin/env python3
"""analyze-u8.py -- find signals in an ogn-rf raw capture.

ogn-rf writes 8-bit unsigned interleaved I/Q (`.u8`) when `RF.OGN.SaveRawData`
is set. This reads such a file, averages a Welch spectrum AND keeps a max-hold
spectrum (OGN/FLARM and most ISM traffic is bursty - averaging alone hides it),
and lists the strongest carriers with their SNR over the median noise floor.

Usage:
    ./analyze-u8.py TEST0_2026.11.08.u8 --centre 868.3 --rate 1000000
    ./analyze-u8.py capture.u8 -c 868.3 -r 1e6 --dc-khz 25

Two artifacts are excluded by default because they are receiver, not RF:
  * the DC/LO spike at the exact centre frequency  (--dc-khz, default 25)
  * the outer 5% of the band, where the RTL-SDR digital downconversion leaves
    a fixed artefact around -983 kHz that TRACKS THE TUNER. Confirm any candidate
    by retuning: a real carrier stays at a fixed absolute frequency, an artefact
    keeps its offset from centre.
"""
import argparse
import sys

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("-c", "--centre", type=float, default=868.3,
                    help="centre frequency in MHz (default 868.3)")
    ap.add_argument("-r", "--rate", type=float, default=1e6,
                    help="sample rate in Hz (default 1e6)")
    ap.add_argument("-n", "--fft", type=int, default=4096)
    ap.add_argument("--dc-khz", type=float, default=25.0)
    ap.add_argument("--edge-pct", type=float, default=5.0)
    ap.add_argument("--top", type=int, default=8)
    a = ap.parse_args()

    raw = np.fromfile(a.file, dtype=np.uint8)
    if raw.size < a.fft * 2:
        sys.exit("file too short")
    iq = (raw.astype(np.float32) - 127.5) / 128.0
    z = iq[0::2] + 1j * iq[1::2]

    print(f"file        : {a.file}")
    print(f"samples     : {z.size} IQ pairs ({z.size / a.rate:.3f} s at {a.rate/1e6:.3f} Msps)")
    print(f"mean |z|    : {np.mean(np.abs(z)):.5f}   std: {np.std(np.abs(z)):.5f}")
    print(f"centre      : {a.centre:.4f} MHz")

    n = a.fft
    segs = z[: z.size // n * n].reshape(-1, n)
    if segs.shape[0] < 2:
        sys.exit("not enough full segments")
    w = np.hanning(n)
    F = np.fft.fftshift(np.fft.fft(segs * w, axis=1), axes=1)
    P = np.abs(F) ** 2
    avg, mx = P.mean(axis=0), P.max(axis=0)

    med = np.median(avg)
    avg_db = 10 * np.log10(np.maximum(avg, 1e-20) / max(med, 1e-20))
    max_db = 10 * np.log10(np.maximum(mx, 1e-20) / max(med, 1e-20))

    bins = np.arange(n) - n // 2
    freqs = a.centre * 1e6 + bins * (a.rate / n)

    dc_bins = int(a.dc_khz * 1e3 / (a.rate / n) + 0.5)
    edge = int(n * a.edge_pct / 100.0)
    mask = (np.abs(bins) > dc_bins) & (np.arange(n) >= edge) & (np.arange(n) < n - edge)

    print(f"segments    : {segs.shape[0]} x {n} pt")
    print(f"excluded    : DC +-{a.dc_khz:.0f} kHz, outer {a.edge_pct:.0f}% of band\n")

    for label, db in (("continuous (averaged)", avg_db), ("bursts (max-hold)", max_db)):
        masked = np.where(mask, db, -999.0)
        idx = np.argsort(masked)[::-1][: a.top]
        print(f"{label}:")
        for k in idx:
            if masked[k] < 1.0:
                continue
            print(f"   {freqs[k]/1e6:11.4f} MHz  {masked[k]:6.1f} dB")
        print()


if __name__ == "__main__":
    main()
