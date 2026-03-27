#!/usr/bin/env python3
"""Detailed phase timing analysis for audiotest7.wav"""
import numpy as np
from scipy.io import wavfile
from numpy.fft import rfft, rfftfreq

sr, data = wavfile.read('audiotest7.wav')
data = data.astype(np.float64)

print(f"File: {len(data)} samples, {sr} Hz, {len(data)/sr:.2f}s")

# Scan for energy + dominant frequency in 0.5s windows
print("\n=== Detailed 0.5s scan (only loud parts) ===")
print(f"{'Time':>6s}  {'RMS dB':>7s}  {'DomFreq':>8s}  {'Phase guess':>20s}")

# Expected phases (with correct TT_RATE=44100):
# SYNC:    1.0s  (2kHz beeps)
# SILENCE: 4.0s
# 1kHz-30: 4.0s
# 1kHz-24: 4.0s
# 1kHz-18: 4.0s
# 1kHz-12: 4.0s
# 440Hz:   4.0s
# SWEEP:   8.0s
# SQUARE:  4.0s
# IMD:     4.0s
# IMPULSE: 4.0s
# Total:  45.0s

phase_boundaries = []  # (time, phase_name)

for t in np.arange(0, len(data)/sr, 0.5):
    s = int(t * sr)
    e = min(int((t + 0.5) * sr), len(data))
    chunk = data[s:e]
    rms = np.sqrt(np.mean(chunk**2))
    rms_db = 20*np.log10(rms/32768 + 1e-12)
    
    if rms_db > -40:
        spec = np.abs(rfft(chunk))
        f = rfftfreq(len(chunk), 1/sr)
        dom_idx = np.argmax(spec[5:]) + 5
        dom = f[dom_idx]
        
        # Guess the phase
        guess = ""
        if 1800 < dom < 2200: guess = "SYNC 2kHz"
        elif 900 < dom < 1100 and rms_db > -25: guess = f"1kHz ({rms_db:.0f}dB)"
        elif 400 < dom < 480 and rms_db > -25: guess = "440Hz"
        elif 100 < dom < 300 and rms_db > -25: guess = f"SWEEP low ({dom:.0f} Hz)"
        elif dom > 1000 and rms_db > -20: guess = f"SWEEP/other ({dom:.0f} Hz)"
        elif rms_db > -25: guess = f"??? ({dom:.0f} Hz)"
        
        print(f"{t:6.1f}s  {rms_db:7.1f}  {dom:8.0f}  {guess:>20s}")

# Precise frequency measurement at expected 1kHz phases
print("\n=== Precise frequency at key phases ===")
# From diagnose output: signal starts around t=11
test_regions = [
    ("SYNC?", 11.0, 11.5),
    ("1kHz-30?", 15.0, 17.0),
    ("1kHz-24?", 19.0, 21.0),
    ("1kHz-18?", 23.0, 25.0),
    ("1kHz-12?", 27.0, 29.0),
    ("440Hz?", 29.0, 31.0),
    ("SWEEP start", 31.0, 32.0),
    ("SWEEP end", 35.0, 36.0),
    ("SQUARE?", 36.0, 38.0),
    ("IMD?", 39.0, 41.0),
    ("IMPULSE?", 42.0, 44.0),
]

for name, t0, t1 in test_regions:
    s = int(t0 * sr)
    e = int(t1 * sr)
    chunk = data[s:e]
    rms = np.sqrt(np.mean(chunk**2))
    rms_db = 20*np.log10(rms/32768 + 1e-12)
    
    spec = np.abs(rfft(chunk))
    f = rfftfreq(len(chunk), 1/sr)
    dom_idx = np.argmax(spec[5:]) + 5
    dom = f[dom_idx]
    
    top5_idx = np.argsort(spec[5:])[-5:][::-1] + 5
    top5_str = ", ".join(f"{f[i]:.0f}" for i in top5_idx)
    
    print(f"  {name:15s} ({t0:.0f}-{t1:.0f}s): dom={dom:7.1f} Hz, rms={rms_db:.1f} dB, top5: {top5_str}")

# Compute total test duration
print("\n=== Timing analysis ===")
# Find first loud moment (test start)
for t in np.arange(0, 20, 0.05):
    s = int(t * sr)
    e = int((t + 0.05) * sr)
    chunk = data[s:e]
    rms = np.sqrt(np.mean(chunk**2))
    rms_db = 20*np.log10(rms/32768 + 1e-12)
    if rms_db > -25:
        test_start = t
        print(f"First loud signal at t={t:.2f}s (rms={rms_db:.1f} dB)")
        break

# Find last loud moment (test end)
for t in np.arange(len(data)/sr - 1, 0, -0.05):
    s = max(0, int(t * sr))
    e = int((t + 0.05) * sr)
    if e > len(data): e = len(data)
    chunk = data[s:e]
    rms = np.sqrt(np.mean(chunk**2))
    rms_db = 20*np.log10(rms/32768 + 1e-12)
    if rms_db > -25:
        test_end = t + 0.05
        print(f"Last loud signal at t={test_end:.2f}s (rms={rms_db:.1f} dB)")
        break

duration = test_end - test_start
expected_duration = 45.0
print(f"Total test duration: {duration:.2f}s (expected {expected_duration:.1f}s)")
print(f"Ratio: {duration/expected_duration:.4f}x")
print(f"Effective playback rate: {44100 * expected_duration / duration:.0f} Hz")
