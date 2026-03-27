#!/usr/bin/env python3
"""Diagnose test tone recording: look at raw samples and auto-detect phase boundaries."""
import sys
import numpy as np
import wave

fname = sys.argv[1] if len(sys.argv) > 1 else 'audiotest_stereo.wav'
print(f"Analyzing: {fname}")

# Handle AIFC via ffmpeg conversion
if fname.lower().endswith(('.aifc', '.aif', '.aiff', '.m4a')):
    import subprocess, os
    wav_path = fname.rsplit('.', 1)[0] + '.wav'
    if not os.path.exists(wav_path):
        print(f"Converting {fname} -> {wav_path}")
        subprocess.run(['ffmpeg', '-y', '-i', fname, '-acodec', 'pcm_s16le', wav_path],
                       check=True, capture_output=True)
    fname = wav_path

with wave.open(fname, 'rb') as wf:
    sr = wf.getframerate()
    nch = wf.getnchannels()
    n = wf.getnframes()
    raw = np.frombuffer(wf.readframes(n), dtype=np.int16)
    data = raw.reshape(-1, nch)
    mono = data[:, 0].astype(np.float64)

print(f"File: {n} frames, {nch} ch, {sr} Hz, {n/sr:.2f}s")
print(f"Ch0 peak: {np.max(np.abs(mono)):.0f} ({20*np.log10(np.max(np.abs(mono))/32768+1e-15):.1f} dBFS)")
print()

# Compute RMS energy in 100ms windows
win = sr // 10  # 4410 samples
n_windows = len(mono) // win
rms = np.zeros(n_windows)
for i in range(n_windows):
    chunk = mono[i*win:(i+1)*win]
    rms[i] = np.sqrt(np.mean(chunk**2))

rms_db = 20 * np.log10(rms / 32768 + 1e-15)

# Find energy transitions (phase boundaries)
print("=== Energy transitions (>6 dB change in 100ms) ===")
print(f"{'Time':>7s}  {'RMS dB':>7s}  {'Delta':>6s}  Note")
prev_db = rms_db[0]
for i in range(1, len(rms_db)):
    delta = rms_db[i] - prev_db
    t = i * win / sr
    if abs(delta) > 6:
        note = "RISE" if delta > 0 else "DROP"
        print(f"{t:7.1f}  {rms_db[i]:7.1f}  {delta:+6.1f}  {note}")
    prev_db = rms_db[i]

# Dominant frequency (FFT-based) in 500ms windows
from scipy.fft import rfft, rfftfreq

print()
print("=== Dominant frequency (FFT) every 0.5s ===")
print(f"{'Time':>7s}  {'DomFreq':>8s}  {'RMS dB':>7s}  {'Peak dB':>7s}  Top frequencies")
win2 = sr // 2  # 500ms windows
max_t = n / sr
for start_s in np.arange(0.0, max_t, 0.5):
    start = int(start_s * sr)
    end = min(start + win2, len(mono))
    if end - start < win2 // 2:
        break
    chunk = mono[start:end]

    chunk_rms = np.sqrt(np.mean(chunk**2))
    chunk_peak = np.max(np.abs(chunk))
    rms_db_val = 20 * np.log10(chunk_rms / 32768 + 1e-15)
    peak_db_val = 20 * np.log10(chunk_peak / 32768 + 1e-15)

    # FFT
    N = len(chunk)
    w = np.hanning(N)
    spec = np.abs(rfft(chunk / 32768.0 * w))
    freqs = rfftfreq(N, 1 / sr)
    # Skip DC
    dom_idx = np.argmax(spec[5:]) + 5
    dom_f = freqs[dom_idx]

    # Top 3
    top_idx = np.argsort(spec[5:])[-3:][::-1] + 5
    top_str = ", ".join(f"{freqs[i]:.0f}Hz" for i in top_idx)

    # Only print rows with signal
    if rms_db_val > -70:
        print(f"{start_s:7.1f}  {dom_f:8.0f}  {rms_db_val:7.1f}  {peak_db_val:7.1f}  {top_str}")
    elif int(start_s * 2) % 10 == 0:  # print quiet rows every 5s
        print(f"{start_s:7.1f}  {'(quiet)':>8s}  {rms_db_val:7.1f}  {peak_db_val:7.1f}")

# Summary: expected vs observed phase map
print()
print("=== Expected phases (from firmware, TT_RATE=49716) ===")
phases = [
    ("SYNC 2kHz beeps", 1.0),
    ("SILENCE", 4.0),
    ("1kHz -30dBFS", 4.0),
    ("1kHz -24dBFS", 4.0),
    ("1kHz -18dBFS", 4.0),
    ("1kHz -12dBFS", 4.0),
    ("440Hz -24dBFS", 4.0),
    ("SWEEP 100-20kHz", 8.0),
    ("SQUARE 1kHz", 4.0),
    ("IMD 19k+20k", 4.0),
    ("IMPULSE 200Hz", 4.0),
]
# The 49716 Hz samples are resampled to 44100 Hz on P4, so
# wall-clock duration = firmware_samples / 49716
# But total_duration should match since both are in seconds
t_offset = 0
print(f"  {'Phase':<25s}  {'Duration':>8s}  {'Cumulative':>10s}")
for name, dur in phases:
    print(f"  {name:<25s}  {dur:8.1f}s  {t_offset:10.1f}s")
    t_offset += dur
print(f"  {'TOTAL':<25s}  {t_offset:8.1f}s")
