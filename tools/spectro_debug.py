#!/usr/bin/env python3
"""Quick spectrogram analysis of test tone recording."""
import numpy as np
import wave
from scipy import signal as sig

with wave.open('audiotest_stereo.wav', 'rb') as wf:
    sr = wf.getframerate()
    n = wf.getnframes()
    raw = np.frombuffer(wf.readframes(n), dtype=np.int16)
    data = raw.reshape(-1, wf.getnchannels())
    mono = data[:, 0].astype(np.float64) / 32768.0

# Compute spectrogram with 50ms windows, 25ms overlap
nperseg = int(0.05 * sr)
f, t, Sxx = sig.spectrogram(mono, sr, nperseg=nperseg, noverlap=nperseg // 2)
Sxx_db = 10 * np.log10(Sxx + 1e-20)

# Print dominant freq + level at each time step — every 0.5s for overview
print(f"{'Time':>7s}  {'DomFreq':>8s}  {'Level':>6s}  {'500Hz':>6s}  {'1kHz':>6s}  {'2kHz':>6s}  {'440Hz':>6s}")
prev_t = -1
for i, ti in enumerate(t):
    if ti < 10:
        continue
    # Print every 0.25s
    if ti - prev_t < 0.24:
        continue
    prev_t = ti
    col = Sxx_db[:, i]
    dom_idx = np.argmax(col)
    dom_freq = f[dom_idx]
    dom_level = col[dom_idx]
    i500 = np.argmin(np.abs(f - 500))
    i1k = np.argmin(np.abs(f - 1000))
    i2k = np.argmin(np.abs(f - 2000))
    i440 = np.argmin(np.abs(f - 440))
    print(f"{ti:7.2f}  {dom_freq:8.0f}  {dom_level:6.1f}  "
          f"{col[i500]:6.1f}  {col[i1k]:6.1f}  {col[i2k]:6.1f}  {col[i440]:6.1f}")
