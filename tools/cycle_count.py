#!/usr/bin/env python3
"""Count actual cycles in a known-signal region to verify true frequency."""
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
    mono = data[:, 0].astype(np.float64)

print(f"File: {fname}, {n/sr:.1f}s, {nch} ch, {sr} Hz")
print()

# Look at a clean 1kHz region (expected ~25s in the recording based on diagnostics)
# The -24dBFS 1kHz phase should be around t=22.5-24.5s based on the diagnostic
from scipy.fft import rfft, rfftfreq

def analyze_region(mono, sr, t_start, t_end, label):
    """Analyze a time region: FFT, zero-crossings, cycle counting."""
    s0 = int(t_start * sr)
    s1 = int(t_end * sr)
    chunk = mono[s0:s1]
    N = len(chunk)
    
    # RMS and peak
    rms = np.sqrt(np.mean(chunk**2))
    peak = np.max(np.abs(chunk))
    rms_db = 20 * np.log10(rms / 32768 + 1e-15)
    peak_db = 20 * np.log10(peak / 32768 + 1e-15)
    
    # FFT - find dominant frequency precisely
    w = np.hanning(N)
    spec = np.abs(rfft(chunk / 32768.0 * w))
    freqs = rfftfreq(N, 1 / sr)
    # Skip DC and very low frequencies
    dom_idx = np.argmax(spec[10:]) + 10
    dom_freq = freqs[dom_idx]
    
    # Zero-crossing count for verification
    signs = np.sign(chunk)
    zc = np.sum(np.abs(np.diff(signs)) > 0)
    zcr_freq = zc / 2.0 / ((s1 - s0) / sr)
    
    # Count individual cycles by finding positive-going zero crossings
    pos_crossings = []
    for i in range(1, len(chunk)):
        if chunk[i-1] <= 0 and chunk[i] > 0:
            # Linear interpolation for sub-sample accuracy
            frac = -chunk[i-1] / (chunk[i] - chunk[i-1] + 1e-15)
            pos_crossings.append(i - 1 + frac)
    
    if len(pos_crossings) > 2:
        periods = np.diff(pos_crossings)
        avg_period = np.mean(periods)
        cycle_freq = sr / avg_period
        period_std = np.std(periods)
        jitter_pct = period_std / avg_period * 100
    else:
        cycle_freq = 0
        jitter_pct = 0
        avg_period = 0
    
    # Top 5 spectral peaks
    top_idx = np.argsort(spec[5:])[-5:][::-1] + 5
    top_str = ", ".join(f"{freqs[i]:.1f}Hz" for i in top_idx)
    
    print(f"--- {label} (t={t_start:.1f}-{t_end:.1f}s) ---")
    print(f"  RMS: {rms_db:.1f} dBFS, Peak: {peak_db:.1f} dBFS")
    print(f"  FFT dominant: {dom_freq:.1f} Hz")
    print(f"  ZCR frequency: {zcr_freq:.1f} Hz")
    print(f"  Cycle-count freq: {cycle_freq:.1f} Hz (period={avg_period:.2f} samples, jitter={jitter_pct:.2f}%)")
    print(f"  Top-5 spectral: {top_str}")
    print(f"  Cycles in region: {len(pos_crossings)}")
    print()
    return dom_freq

# Analyze specific regions based on the diagnostic output
regions = [
    (18.0, 18.4, "SYNC beeps (expect 2000 Hz)"),
    (21.0, 22.0, "Phase 2 early (expect 1000 Hz at -30 dBFS)"),
    (23.0, 24.0, "Phase 3 (expect 1000 Hz at -24 dBFS)"),
    (25.0, 26.0, "Phase 4 (expect 1000 Hz at -18 dBFS)"),
    (27.0, 28.0, "Phase 5 (expect 1000 Hz at -12 dBFS)"),
    (29.0, 30.0, "Phase 6 (expect 440 Hz at -24 dBFS)"),
    (35.0, 36.0, "SQUARE (expect 1000 Hz)"),
]

observed_freqs = []
for t0, t1, label in regions:
    if t1 * sr < len(mono):
        f = analyze_region(mono, sr, t0, t1, label)
        observed_freqs.append((label, f))

# Compute ratios
print("=== Frequency Ratio Analysis ===")
expected = [2000, 1000, 1000, 1000, 1000, 440, 1000]
for i, ((label, obs), exp) in enumerate(zip(observed_freqs, expected)):
    ratio = obs / exp
    print(f"  {label}: {obs:.1f}/{exp} = {ratio:.4f}x")

# Raw waveform dump: first 100 samples of a 1kHz region
print()
print("=== Raw samples at t=23.5s (first 50 samples, expect 1kHz sine) ===")
start = int(23.5 * sr)
for i in range(50):
    val = mono[start + i]
    bar = '#' * int(abs(val) / 500)
    sign = '+' if val >= 0 else '-'
    print(f"  [{i:3d}] {val:7.0f}  {sign}{bar}")
