#!/usr/bin/env python3
"""Simulate P4 ring buffer with actual ring_push overflow drop logic.

Matches ctagSoundProcessorPicoAudioBridge.cpp exactly:
- ring_push: drops oldest when overflow (advances ring_rd)
- ring is STEREO (ring_buf[wr*2], ring_buf[wr*2+1])
- direct_pull: reads min(avail, 32) stereo pairs
- RING_SIZE=4096 stereo pair slots
"""
import numpy as np
from numpy.fft import rfft, rfftfreq

RING_SIZE = 4096
RING_MASK = RING_SIZE - 1
RATE = 44100
BUF_SZ = 32
PUSH_COUNT = 62  # stereo pairs per SPI frame from RP2350
FREQ = 1000
DURATION = 3  # seconds

# Generate input: stereo int16 pairs (L=R for test tones)
total_pairs = RATE * DURATION
phase = np.cumsum(np.full(total_pairs, FREQ / RATE))
input_L = (2067.0 * np.sin(2 * np.pi * phase)).astype(np.int16)
input_R = input_L.copy()  # mono test: L=R

# P4 ring buffer (int16 stereo, matching actual code)
ring_buf = np.zeros(RING_SIZE * 2, dtype=np.int16)
ring_wr = 0
ring_rd = 0

def ring_available():
    return (ring_wr - ring_rd) & RING_MASK

def ring_free():
    return (RING_SIZE - 1) - ring_available()

def ring_push(L_samples, R_samples, count):
    global ring_wr, ring_rd
    free = ring_free()
    if count > free:
        drop = count - free
        ring_rd = (ring_rd + drop) & RING_MASK
    for i in range(count):
        ring_buf[ring_wr * 2] = L_samples[i]
        ring_buf[ring_wr * 2 + 1] = R_samples[i]
        ring_wr = (ring_wr + 1) & RING_MASK

def direct_pull(out_count):
    global ring_rd
    avail = ring_available()
    count = min(out_count, avail)
    out_L = np.zeros(out_count, dtype=np.float32)
    out_R = np.zeros(out_count, dtype=np.float32)
    scale = 1.0 / 32768.0
    for i in range(count):
        out_L[i] = ring_buf[ring_rd * 2] * scale
        out_R[i] = ring_buf[ring_rd * 2 + 1] * scale
        ring_rd = (ring_rd + 1) & RING_MASK
    return out_L, out_R

# Simulate: push PUSH_COUNT, pull BUF_SZ per codec cycle
output_L = []
in_idx = 0
n_cycles = total_pairs // PUSH_COUNT

for cycle in range(n_cycles):
    remaining = total_pairs - in_idx
    count = min(PUSH_COUNT, remaining)
    if count <= 0:
        break
    ring_push(input_L[in_idx:in_idx+count], input_R[in_idx:in_idx+count], count)
    in_idx += count
    
    L, R = direct_pull(BUF_SZ)
    # Apply x8 gain + clamp (matching P4)
    L = np.clip(L * 8.0, -1.0, 1.0)
    output_L.extend(L)

output = np.array(output_L, dtype=np.float32)

# Analyze
skip = RATE  # skip first 1s
analysis = output[skip:]
spectrum = np.abs(rfft(analysis))
freqs = rfftfreq(len(analysis), 1.0 / RATE)
dom_idx = np.argmax(spectrum[10:]) + 10

print(f"=== P4 Ring Simulation (actual overflow drop logic) ===")
print(f"Input: {FREQ} Hz sine at {RATE} Hz, stereo (L=R)")
print(f"Push: {PUSH_COUNT} pairs/cycle, Pull: {BUF_SZ} pairs/cycle")
print(f"Output: {len(output)} samples ({len(output)/RATE:.2f}s)")
print(f"Dominant freq: {freqs[dom_idx]:.1f} Hz")
print(f"Ratio: {freqs[dom_idx]/FREQ:.4f}")
print(f"Ring overflow drops: {PUSH_COUNT - BUF_SZ} pairs per cycle in steady state")
print()

# Top 5 frequencies
top5 = np.argsort(spectrum[10:])[-5:][::-1] + 10
print(f"Top 5 frequencies: {', '.join(f'{freqs[i]:.0f} Hz' for i in top5)}")
print()

# Check waveform at block boundaries
print(f"=== Block boundary samples (output) ===")
for b in range(5, 10):
    i = b * BUF_SZ
    pre = output[i-3:i]
    post = output[i:i+3]
    delta = post[0] - pre[-1] if len(pre) > 0 and len(post) > 0 else 0
    print(f"  Block {b}: ...{pre[-3:]!r} | {post[:3]!r}  Δ={delta:+.4f}")

# Simulate with cap at 32
print(f"\n=== With push capped at 32 ===")
ring_buf2 = np.zeros(RING_SIZE * 2, dtype=np.int16)
ring_wr = 0
ring_rd = 0
output2_L = []
in_idx = 0

for cycle in range(n_cycles):
    remaining = total_pairs - in_idx
    count = min(32, remaining)  # CAP AT 32
    if count <= 0:
        break
    ring_push(input_L[in_idx:in_idx+count], input_R[in_idx:in_idx+count], count)
    in_idx += count
    L, R = direct_pull(BUF_SZ)
    L = np.clip(L * 8.0, -1.0, 1.0)
    output2_L.extend(L)

output2 = np.array(output2_L, dtype=np.float32)
analysis2 = output2[skip:]
spectrum2 = np.abs(rfft(analysis2))
freqs2 = rfftfreq(len(analysis2), 1.0 / RATE)
dom_idx2 = np.argmax(spectrum2[10:]) + 10
print(f"Dominant freq: {freqs2[dom_idx2]:.1f} Hz (ratio: {freqs2[dom_idx2]/FREQ:.4f})")
top5b = np.argsort(spectrum2[10:])[-5:][::-1] + 10
print(f"Top 5: {', '.join(f'{freqs2[i]:.0f} Hz' for i in top5b)}")
