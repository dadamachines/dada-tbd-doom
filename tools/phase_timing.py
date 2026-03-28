#!/usr/bin/env python3
"""
Auto-detecting phase timing analysis for TBD-16 audio test recordings.

Works regardless of when recording started/stopped. Finds the SYNC 2kHz
beacon automatically and anchors all analysis windows from there.

Usage: python phase_timing.py <recording.wav>
"""
import sys
import numpy as np
from scipy.io import wavfile
from numpy.fft import rfft, rfftfreq

if len(sys.argv) < 2:
    print(f"Usage: {sys.argv[0]} <recording.wav>")
    sys.exit(1)

fname = sys.argv[1]
sr, raw = wavfile.read(fname)
if raw.ndim == 2:
    data = raw[:, 0].astype(np.float64)
else:
    data = raw.astype(np.float64)
total_sec = len(data) / sr
print(f"File: {fname}")
print(f"  {len(data)} samples, {sr} Hz, {total_sec:.2f}s, "
      f"{'stereo (ch0)' if raw.ndim == 2 else 'mono'}")


def analyze_chunk(chunk, sr):
    """Return (rms_db, dominant_freq, top5_list) for a chunk."""
    rms = np.sqrt(np.mean(chunk**2))
    rms_db = 20 * np.log10(rms / 32768 + 1e-12)
    spec = np.abs(rfft(chunk))
    f = rfftfreq(len(chunk), 1 / sr)
    dom_idx = np.argmax(spec[5:]) + 5
    dom = f[dom_idx]
    top5_idx = np.argsort(spec[5:])[-5:][::-1] + 5
    top5 = [(f[i], spec[i]) for i in top5_idx]
    return rms_db, dom, top5


# ---- Step 1: Auto-detect SYNC 2kHz beacon ----------------------------
print("\n=== Auto-detecting test start (SYNC 2kHz beacon) ===")
sync_start = None
win = 0.1
for t in np.arange(0, total_sec - win, 0.02):
    s, e = int(t * sr), int((t + win) * sr)
    rms_db, dom, _ = analyze_chunk(data[s:e], sr)
    if rms_db > -30 and 1800 < dom < 2200:
        sync_start = t
        print(f"  SYNC 2kHz found at t={t:.2f}s "
              f"(dom={dom:.0f} Hz, rms={rms_db:.1f} dB)")
        break

if sync_start is None:
    for t in np.arange(0, total_sec - 0.05, 0.02):
        s, e = int(t * sr), int((t + 0.05) * sr)
        rms_db, _, _ = analyze_chunk(data[s:e], sr)
        if rms_db > -25:
            sync_start = t
            print(f"  No SYNC found -- first loud at t={t:.2f}s")
            break

if sync_start is None:
    print("  ERROR: No signal found in recording!")
    sys.exit(1)

T = sync_start
print(f"  Anchor T = {T:.2f}s")

# ---- Phase layout (from firmware test_tone_generate) ------------------
#   All offsets relative to SYNC start
PHASES = [
    ("SYNC 2kHz",     0,   1,  2000),
    ("SILENCE",       1,   5,  None),
    ("1kHz -30dB",    5,   9,  1000),
    ("1kHz -24dB",    9,  13,  1000),
    ("1kHz -18dB",   13,  17,  1000),
    ("1kHz -12dB",   17,  21,  1000),
    ("440Hz -24dB",  21,  25,   440),
    ("SWEEP 100-20k",25,  33,  None),
    ("SQUARE 1kHz",  33,  37,  1000),
    ("IMD 19k+20k",  37,  41, 19000),
    ("IMPULSE 200Hz",41,  45,   200),
]

# ---- Step 2: Phase-by-phase analysis ---------------------------------
hdr = (f"{'Phase':<16s} {'Window':>13s}  {'DomFreq':>8s} "
       f"{'Expected':>8s} {'RMS dB':>7s}  {'Match':>5s}  Top frequencies")
print(f"\n=== Phase analysis (anchored to T={T:.2f}s) ===")
print(hdr)
print("-" * 100)

results = []
for name, t0_off, t1_off, expected_freq in PHASES:
    margin = (t1_off - t0_off) * 0.1
    t0 = T + t0_off + margin
    t1 = T + t1_off - margin
    if t1 > total_sec:
        print(f"{name:<16s}  {'BEYOND RECORDING':>13s}")
        results.append((name, None, None, None))
        continue

    s, e = int(t0 * sr), int(t1 * sr)
    rms_db, dom, top5 = analyze_chunk(data[s:e], sr)
    top5_str = ", ".join(f"{f:.0f}" for f, _ in top5)

    if expected_freq is None:
        if name.startswith("SILENCE"):
            match = "OK" if rms_db < -50 else "FAIL"
        else:
            match = "--"
    elif name.startswith("SWEEP"):
        match = "OK" if rms_db > -25 else "FAIL"
    elif abs(dom - expected_freq) / expected_freq < 0.05:
        match = "OK"
    else:
        match = "FAIL"

    exp_str = str(expected_freq) if expected_freq else "--"
    window_str = f"{t0:.1f}-{t1:.1f}s"
    print(f"{name:<16s} {window_str:>13s}  {dom:8.0f} {exp_str:>8s}  "
          f"{rms_db:7.1f}  {match:>5s}  {top5_str}")
    results.append((name, dom, rms_db, match))

# ---- Step 3: Level progression (1kHz tones) --------------------------
print("\n=== Level progression (1kHz tones) ===")
level_phases = [(n, d, r) for n, d, r, m in results
                if n.startswith("1kHz") and r is not None]
if len(level_phases) >= 2:
    print(f"  {'Phase':<16s} {'RMS dB':>7s}  {'Delta':>6s}")
    prev_rms = None
    for name, dom, rms_db in level_phases:
        delta = f"{rms_db - prev_rms:+.1f}" if prev_rms is not None else "--"
        print(f"  {name:<16s} {rms_db:7.1f}  {delta:>6s}")
        prev_rms = rms_db
    spread = level_phases[-1][2] - level_phases[0][2]
    print(f"  Total spread: {spread:+.1f} dB (expected ~+18 dB)")

# ---- Step 4: Timing analysis -----------------------------------------
print("\n=== Timing analysis ===")

test_end = None
for t in np.arange(total_sec - 0.5, T, -0.05):
    s = max(0, int(t * sr))
    e = min(int((t + 0.1) * sr), len(data))
    rms_db, _, _ = analyze_chunk(data[s:e], sr)
    if rms_db > -45:
        test_end = t + 0.1
        break
if test_end is None:
    test_end = total_sec

duration = test_end - T
expected = 45.0
ratio = duration / expected
eff_rate = sr / ratio

print(f"  Test start:      t = {T:.2f}s")
print(f"  Test end:        t = {test_end:.2f}s")
print(f"  Test duration:   {duration:.2f}s (expected {expected:.1f}s)")
print(f"  Ratio:           {ratio:.4f}x")
print(f"  Effective rate:  {eff_rate:.0f} Hz (target {sr} Hz)")

if 0.98 <= ratio <= 1.02:
    verdict = "TIMING CORRECT (within +/-2%)"
elif ratio < 0.98:
    verdict = f"TIMING COMPRESSED -- audio too fast ({ratio:.2f}x)"
else:
    verdict = f"TIMING STRETCHED -- audio too slow ({ratio:.2f}x)"
print(f"  Verdict:         {verdict}")

# ---- Step 5: Summary -------------------------------------------------
n_ok   = sum(1 for _, _, _, m in results if m == "OK")
n_fail = sum(1 for _, _, _, m in results if m == "FAIL")
n_skip = sum(1 for _, _, _, m in results if m is None)
n_na   = sum(1 for _, _, _, m in results if m == "--")
testable = len(results) - n_na - n_skip

print(f"\n=== Summary ===")
print(f"  Phases OK:     {n_ok}/{testable}")
print(f"  Phases FAIL:   {n_fail}/{testable}")
print(f"  Skipped:       {n_skip + n_na}")
print(f"  Timing:        {verdict}")
if n_fail == 0 and 0.98 <= ratio <= 1.02:
    print(f"  OVERALL: PASS")
else:
    print(f"  OVERALL: NEEDS ATTENTION")
