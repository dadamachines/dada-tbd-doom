#!/usr/bin/env python3
"""Examine click artifacts at sample level."""
import wave, struct, numpy as np

with wave.open('doomaudio.wav', 'rb') as wf:
    sr = wf.getframerate()
    raw = wf.readframes(wf.getnframes())
    samples = np.array(struct.unpack(f'<{wf.getnframes()*2}h', raw), dtype=np.float64).reshape(-1, 2)

mono = (samples[:,0] + samples[:,1]) / 2.0
mono_norm = mono / 32768.0

diff = np.abs(np.diff(mono_norm))
click_times = [9.629, 9.661, 11.415, 12.129, 13.738, 15.746, 16.556, 16.811]

print("CLICK DETAIL ANALYSIS")
print("=" * 80)
for ct in click_times:
    idx = int(ct * sr)
    start = max(0, idx - 50)
    end = min(len(diff), idx + 50)
    local_diff = diff[start:end]
    peak_off = np.argmax(local_diff)
    peak_idx = start + peak_off
    jump_mag = diff[peak_idx]
    before = mono[peak_idx]
    after_val = mono[peak_idx + 1]
    # Show 5 samples around the discontinuity
    ctx_start = max(0, peak_idx - 3)
    ctx_end = min(len(mono), peak_idx + 4)
    context = [f"{mono[i]:.0f}" for i in range(ctx_start, ctx_end)]
    print(f"t={ct:.3f}s  sample={peak_idx}  before={before:.0f}  after={after_val:.0f}  "
          f"jump={after_val-before:.0f}  mag={jump_mag:.5f}")
    print(f"  context: [{', '.join(context)}]")
    # Check if it's a jump to/from zero
    near_zero = abs(after_val) < 100 or abs(before) < 100
    print(f"  near-zero involved: {near_zero}")
    print()

# Also look at L/R independently for clicks
print("\nSTEREO CLICK CHECK (L vs R at each click)")
print("=" * 80)
for ct in click_times[:4]:
    idx = int(ct * sr)
    start = max(0, idx - 3)
    end = min(len(samples), idx + 4)
    print(f"t={ct:.3f}s:")
    for i in range(start, end):
        l, r = samples[i, 0], samples[i, 1]
        marker = " <-- click" if i == idx else ""
        print(f"  [{i}] L={l:6.0f}  R={r:6.0f}  diff={l-r:6.0f}{marker}")
    print()

# Overall level histogram
print("\nLEVEL DISTRIBUTION")
print("=" * 80)
abs_mono = np.abs(mono)
for threshold_db in [-6, -12, -18, -24, -30, -36, -42, -48]:
    threshold = 32767 * 10**(threshold_db/20)
    pct = 100.0 * np.sum(abs_mono > threshold) / len(abs_mono)
    print(f"  Samples above {threshold_db:4d} dBFS: {pct:6.2f}%")
