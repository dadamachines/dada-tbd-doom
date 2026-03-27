#!/usr/bin/env python3
"""Analyze ISR debug rates from two data points."""

sent1, isr1, busy1, rdy1 = 159652, 576572, 158885, 258035
sent2, isr2, busy2, rdy2 = 180653, 652357, 179782, 291922
dt = 15.0

ds = sent2-sent1; di = isr2-isr1; db = busy2-busy1; dr = rdy2-rdy1
print(f"ISR rate:     {di/dt:.0f} Hz (target 5000)")
print(f"Sent rate:    {ds/dt:.0f} Hz")
print(f"DMA busy:     {db/di*100:.1f}%")
print(f"RDY LOW:      {dr/di*100:.1f}% (was 72%)")
print()
print("--- Throughput with 62 raw samples/frame ---")
raw_rate = ds/dt * 62
print(f"Raw samples/sec at 49716 Hz: {raw_rate:.0f}")
resamp = raw_rate * 44100/49716
print(f"After resample to 44100: {resamp:.0f} samples/sec")
print(f"Codec needs: 44100 samples/sec")
print(f"Headroom: {resamp / 44100:.2f}x")
print()
print("--- Ring buffer problem ---")
print(f"Process()/sec: ~1378 (codec-locked)")
print(f"SPI frames/sec with PCM2 data: {ds/dt:.0f}")
print(f"Each frame pushes 62 samples, resample consumes ~36 per Process()")
no_data = 1378 - ds/dt
print(f"Process() calls with NO new data: {no_data:.0f}/sec -> output SILENCE")
print(f"But ring buffer has buffered samples that should be drained!")
print(f"Net surplus per frame: 62 - 36 = 26 samples")
print(f"Ring buffer size: 256")
print(f"Overflow in ~{256/26:.0f} consecutive frames = {256/26/1378*1000:.1f} ms")
print()
print("ROOT CAUSE: Plugin outputs silence when no new SPI data arrives,")
print("instead of continuing to drain the ring buffer.")
