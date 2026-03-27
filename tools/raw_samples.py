#!/usr/bin/env python3
"""Deep raw sample analysis to find the interleaving/duplication pattern."""
import sys
import numpy as np
import wave

fname = sys.argv[1] if len(sys.argv) > 1 else 'audiotest2.wav'

with wave.open(fname, 'rb') as wf:
    sr = wf.getframerate()
    nch = wf.getnchannels()
    n = wf.getnframes()
    raw = np.frombuffer(wf.readframes(n), dtype=np.int16)
    data = raw.reshape(-1, nch)

print(f"File: {fname}, {n/sr:.1f}s, {nch} ch")

# Dump L and R channels side by side for 200 samples at t=23.5s
# This is the -24dBFS 1kHz region
start = int(23.5 * sr)
print()
print(f"=== L+R raw samples at t=23.5s (200 samples) ===")
print(f"{'i':>4s}  {'L':>7s}  {'R':>7s}  {'L-R':>7s}  {'dL':>7s}  note")
prev_l = data[start, 0]
for i in range(200):
    idx = start + i
    l = int(data[idx, 0])
    r = int(data[idx, 1])
    diff = l - r
    dl = l - prev_l
    note = ""
    if abs(dl) > 3000:
        note = " <<< BIG JUMP"
    prev_l = l
    print(f"{i:4d}  {l:7d}  {r:7d}  {diff:7d}  {dl:7d}  {note}")

# Also look at sample-to-sample differences to find pattern
print()
print("=== Checking for duplicated/dropped samples ===")
mono = data[start:start+2000, 0].astype(np.float64)

# Find all positive-going zero crossings
crossings = []
for i in range(1, len(mono)):
    if mono[i-1] <= 0 and mono[i] > 0:
        frac = -mono[i-1] / (mono[i] - mono[i-1] + 1e-15)
        crossings.append(i - 1 + frac)

if len(crossings) > 2:
    periods = np.diff(crossings)
    print(f"Periods (in samples) for first 40 cycles:")
    for i, p in enumerate(periods[:40]):
        freq = sr / p
        note = ""
        if p < 20:
            note = " SHORT!"
        elif p > 30:
            note = " LONG!"
        print(f"  cycle {i:3d}: period={p:6.1f} samples ({freq:7.1f} Hz){note}")
    print(f"  Mean period: {np.mean(periods):.2f} samples ({sr/np.mean(periods):.1f} Hz)")
    print(f"  Median period: {np.median(periods):.2f} samples ({sr/np.median(periods):.1f} Hz)")
    print(f"  Std dev: {np.std(periods):.2f} samples")
    
    # Histogram of periods
    print()
    print("Period histogram:")
    hist_bins = np.arange(10, 60, 1)
    counts, edges = np.histogram(periods, bins=hist_bins)
    for i in range(len(counts)):
        if counts[i] > 0:
            bar = '#' * min(counts[i], 50)
            print(f"  {edges[i]:5.0f}-{edges[i+1]:5.0f}: {counts[i]:4d} {bar}")

# Also check: is the signal a sum of two frequencies?
print()
print("=== Auto-correlation peak analysis ===")
# Autocorrelation to find true periodicity
chunk = mono[:2000]
chunk = chunk - np.mean(chunk)  # remove DC
autocorr = np.correlate(chunk, chunk, mode='full')
autocorr = autocorr[len(autocorr)//2:]  # keep positive lags
autocorr = autocorr / autocorr[0]  # normalize

# Find peaks in autocorrelation
from scipy.signal import find_peaks
peaks, props = find_peaks(autocorr, height=0.3, distance=5)
print("Autocorrelation peaks (lag, correlation, frequency):")
for p in peaks[:15]:
    freq = sr / p
    print(f"  lag={p:5d} samples, corr={autocorr[p]:.3f}, freq={freq:.1f} Hz")
