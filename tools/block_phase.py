#!/usr/bin/env python3
"""Block-boundary phase analysis for the 32-sample codec block discontinuity.

Splits the 1kHz test tone segment into 32-sample blocks (matching P4 BUF_SZ),
fits a sine to each block, and measures phase continuity across boundaries.

Also simulates the P4 resampler chain to check if the algorithm itself
introduces artifacts.

Usage: python tools/block_phase.py audiotest2.wav
"""
import sys, struct
import numpy as np
from scipy.optimize import minimize_scalar
from scipy.io import wavfile

BLOCK = 32  # P4 BUF_SZ
FS = 44100  # recording sample rate

# ── Load recording ──────────────────────────────────────────────────────

def load_wav_mono(path):
    """Load WAV, return mono float array at native sample rate."""
    sr, data = wavfile.read(path)
    if data.ndim > 1:
        data = data[:, 0]  # take first channel
    return sr, data.astype(np.float64)


def find_1k_segment(sig, sr, target_freq=1000, window_sec=0.5):
    """Find a stable 1kHz segment by scanning for dominant ~1000 Hz energy."""
    win = int(window_sec * sr)
    best_start = 0
    best_power = 0
    # scan in 0.1s steps after first 15s (skip boot silence)
    for t in np.arange(20.0, 35.0, 0.1):
        start = int(t * sr)
        if start + win > len(sig):
            break
        chunk = sig[start:start + win]
        spec = np.abs(np.fft.rfft(chunk))
        freqs = np.fft.rfftfreq(len(chunk), 1.0 / sr)
        # power near 1000 Hz (±100 Hz) vs total
        mask = (freqs > target_freq * 0.8) & (freqs < target_freq * 1.5)
        # also check near 1730 Hz (the observed distorted freq)
        mask2 = (freqs > 1600) & (freqs < 1900)
        power = np.sum(spec[mask | mask2] ** 2)
        if power > best_power:
            best_power = power
            best_start = start
    return best_start, win


def fit_sine_block(block, sr, f_guess):
    """Fit A*sin(2π*f*t + φ) + C to a short block. Returns (A, f, φ, C)."""
    n = len(block)
    t = np.arange(n) / sr

    # Estimate frequency from zero crossings
    # Use f_guess as starting point
    f = f_guess

    # Fit phase using least-squares for fixed frequency
    # Model: y = A*sin(2πf*t + φ) + C
    # Expand: y = A*sin(φ)*cos(2πf*t) + A*cos(φ)*sin(2πf*t) + C
    # Let a = A*sin(φ), b = A*cos(φ)
    # y = a*cos(2πf*t) + b*sin(2πf*t) + C
    cos_comp = np.cos(2 * np.pi * f * t)
    sin_comp = np.sin(2 * np.pi * f * t)
    ones = np.ones(n)

    M = np.column_stack([cos_comp, sin_comp, ones])
    result = np.linalg.lstsq(M, block, rcond=None)
    a, b, C = result[0]

    A = np.sqrt(a ** 2 + b ** 2)
    phi = np.arctan2(a, b)  # phase such that A*sin(2πft + φ)

    return A, f, phi, C


def expected_phase_at(phi0, f, n_samples, sr):
    """Expected phase after n_samples at frequency f."""
    return phi0 + 2 * np.pi * f * n_samples / sr


# ── P4 Chain Simulation ────────────────────────────────────────────────

def simulate_p4_chain(duration_sec=0.1, source_rate=49716, output_rate=44100,
                       freq=1000.0, amplitude=1036):
    """Simulate the full P4 audio chain and return output samples.

    1. Generate 1kHz sine at 49716 Hz (same as RP2350 tt_fill_buffer)
    2. AA biquad LPF at 20kHz (same as P4 ring_push)
    3. Cubic Hermite resampler 49716→44100 (same as P4 resample_pull)
    4. ×8 gain + clamp (same as P4 Process)

    Processes in blocks of 32 output samples (same as P4 BUF_SZ).
    """
    # Generate source samples
    n_source = int(duration_sec * source_rate) + 256  # extra for priming
    phase = 0.0
    phase_inc = freq / source_rate
    source = np.zeros(n_source, dtype=np.int16)
    for i in range(n_source):
        source[i] = int(amplitude * np.sin(phase * 2 * np.pi))
        phase += phase_inc
        if phase >= 1.0:
            phase -= 1.0

    # Make stereo (L=R)
    stereo_source = np.zeros(n_source * 2, dtype=np.int16)
    stereo_source[0::2] = source
    stereo_source[1::2] = source

    # ── AA Biquad LPF (Butterworth, fc=20kHz, Q=sqrt(2)/2) ──
    fc = 20000.0
    w0 = 2.0 * np.pi * fc / source_rate
    cosw0 = np.cos(w0)
    sinw0 = np.sin(w0)
    alpha = sinw0 / (2.0 * 0.7071067811865476)
    a0 = 1.0 + alpha
    b0 = ((1.0 - cosw0) * 0.5) / a0
    b1 = (1.0 - cosw0) / a0
    b2 = ((1.0 - cosw0) * 0.5) / a0
    a1 = (-2.0 * cosw0) / a0
    a2 = (1.0 - alpha) / a0

    # Apply biquad (Direct Form I, per channel)
    filtered = np.zeros(n_source * 2, dtype=np.float64)
    x1_l = x2_l = y1_l = y2_l = 0.0
    x1_r = x2_r = y1_r = y2_r = 0.0
    ring_buf = np.zeros(4096 * 2, dtype=np.int16)  # P4 ring buffer
    ring_wr = 0
    ring_rd = 0
    RING_SIZE = 4096
    RING_MASK = RING_SIZE - 1
    WATERMARK = 256

    # Push ALL source samples through AA filter into ring buffer
    for i in range(n_source):
        x_l = float(stereo_source[i * 2])
        x_r = float(stereo_source[i * 2 + 1])

        y_l = b0 * x_l + b1 * x1_l + b2 * x2_l - a1 * y1_l - a2 * y2_l
        x2_l, x1_l = x1_l, x_l
        y2_l, y1_l = y1_l, y_l

        y_r = b0 * x_r + b1 * x1_r + b2 * x2_r - a1 * y1_r - a2 * y2_r
        x2_r, x1_r = x1_r, x_r
        y2_r, y1_r = y1_r, y_r

        # Clamp to int16
        y_l = max(-32768.0, min(32767.0, y_l))
        y_r = max(-32768.0, min(32767.0, y_r))

        ring_buf[ring_wr * 2] = int(y_l)
        ring_buf[ring_wr * 2 + 1] = int(y_r)
        ring_wr = (ring_wr + 1) & RING_MASK

    # ── Cubic Hermite Resampler (block-by-block, matching P4) ──
    resample_step = source_rate / output_rate
    resample_pos = 0.0
    n_output = int(duration_sec * output_rate)
    output = np.zeros(n_output * 2, dtype=np.float64)
    scale = 1.0 / 32768.0

    # Wait for watermark (skip priming)
    ring_rd = 0
    avail = (ring_wr - ring_rd) & RING_MASK

    out_idx = 0
    while out_idx < n_output:
        block_size = min(BLOCK, n_output - out_idx)
        avail = (ring_wr - ring_rd) & RING_MASK
        step = resample_step  # no adaptive adjustment in sim

        for j in range(block_size):
            pos = int(resample_pos)
            t = resample_pos - pos

            im1 = (ring_rd + pos - 1) & RING_MASK
            i0 = (ring_rd + pos) & RING_MASK
            i1 = (ring_rd + pos + 1) & RING_MASK
            i2 = (ring_rd + pos + 2) & RING_MASK

            for ch in range(2):
                sm1 = float(ring_buf[im1 * 2 + ch])
                s0 = float(ring_buf[i0 * 2 + ch])
                s1 = float(ring_buf[i1 * 2 + ch])
                s2 = float(ring_buf[i2 * 2 + ch])

                a_coeff = -0.5 * sm1 + 1.5 * s0 - 1.5 * s1 + 0.5 * s2
                b_coeff = sm1 - 2.5 * s0 + 2.0 * s1 - 0.5 * s2
                c_coeff = -0.5 * sm1 + 0.5 * s1

                val = ((a_coeff * t + b_coeff) * t + c_coeff) * t * scale + s0 * scale
                output[(out_idx + j) * 2 + ch] = val

            resample_pos += step

        consumed = int(resample_pos)
        ring_rd = (ring_rd + consumed) & RING_MASK
        resample_pos -= consumed
        out_idx += block_size

    # ×8 gain + clamp
    output *= 8.0
    np.clip(output, -1.0, 1.0, out=output)

    return output[::2]  # return L channel only


# ── Main Analysis ───────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage: python tools/block_phase.py <recording.wav>")
        sys.exit(1)

    fname = sys.argv[1]
    sr, sig = load_wav_mono(fname)
    print(f"Loaded {fname}: {len(sig)} samples, {sr} Hz, {len(sig)/sr:.1f}s")

    # Find 1kHz segment
    seg_start, seg_len = find_1k_segment(sig, sr)
    seg = sig[seg_start:seg_start + seg_len]
    print(f"Using segment at t={seg_start/sr:.2f}s ({seg_len} samples)")

    # ── Block-boundary analysis ──
    n_blocks = seg_len // BLOCK
    print(f"\n{'='*60}")
    print(f"BLOCK-BOUNDARY PHASE ANALYSIS ({n_blocks} blocks of {BLOCK} samples)")
    print(f"{'='*60}")

    # Use the observed frequency (~1730 Hz) for fitting within recording
    # and also try 1000 Hz for comparison
    for fit_freq in [1000, 1730]:
        print(f"\n--- Fitting at {fit_freq} Hz ---")
        phases = []
        amplitudes = []
        for b in range(min(n_blocks, 20)):
            block = seg[b * BLOCK:(b + 1) * BLOCK].astype(np.float64)
            A, f, phi, C = fit_sine_block(block, sr, fit_freq)
            phases.append(phi)
            amplitudes.append(A)

        print(f"{'Block':>5} {'Amplitude':>10} {'Phase(rad)':>12} {'Phase(deg)':>12} {'ΔPhase(deg)':>12} {'Expected Δ':>12} {'Error(deg)':>12}")
        expected_delta = 2 * np.pi * fit_freq * BLOCK / sr
        for b in range(len(phases)):
            phi_deg = np.degrees(phases[b])
            if b > 0:
                dphi = phases[b] - phases[b - 1]
                # Wrap to [-π, π]
                dphi = (dphi + np.pi) % (2 * np.pi) - np.pi
                dphi_deg = np.degrees(dphi)
                expected_deg = np.degrees(expected_delta)
                error_deg = dphi_deg - expected_deg
                # Wrap error to [-180, 180]
                error_deg = (error_deg + 180) % 360 - 180
                print(f"{b:>5} {amplitudes[b]:>10.1f} {phases[b]:>12.4f} {phi_deg:>12.1f} {dphi_deg:>12.1f} {expected_deg:>12.1f} {error_deg:>12.1f}")
            else:
                print(f"{b:>5} {amplitudes[b]:>10.1f} {phases[b]:>12.4f} {phi_deg:>12.1f} {'---':>12} {'---':>12} {'---':>12}")

    # ── Raw sample dump at block boundaries ──
    print(f"\n{'='*60}")
    print(f"RAW SAMPLES AT BLOCK BOUNDARIES")
    print(f"{'='*60}")
    for b in range(min(n_blocks - 1, 15)):
        end_of_block = seg[(b + 1) * BLOCK - 3:(b + 1) * BLOCK]
        start_of_next = seg[(b + 1) * BLOCK:(b + 1) * BLOCK + 3]
        last3 = ', '.join(f'{v:>7.0f}' for v in end_of_block)
        first3 = ', '.join(f'{v:>7.0f}' for v in start_of_next)
        jump = start_of_next[0] - end_of_block[-1]
        marker = " <<<" if abs(jump) > 3000 else ""
        print(f"  Block {b:>2}→{b+1:>2}: [...{last3}] | [{first3}...]  Δ={jump:>+8.0f}{marker}")

    # ── Simulation ──
    print(f"\n{'='*60}")
    print(f"P4 CHAIN SIMULATION (1kHz sine, 49716→44100 Hz)")
    print(f"{'='*60}")

    sim = simulate_p4_chain(duration_sec=0.05, freq=1000.0, amplitude=1036)
    sim_scaled = sim * 32768  # back to int16-ish for comparison

    # Check for discontinuities at block boundaries in simulation
    n_sim_blocks = len(sim_scaled) // BLOCK
    print(f"Simulated {len(sim_scaled)} output samples ({n_sim_blocks} blocks)")
    print(f"\nSimulation block boundaries:")
    max_jump = 0
    for b in range(min(n_sim_blocks - 1, 15)):
        last = sim_scaled[(b + 1) * BLOCK - 1]
        first = sim_scaled[(b + 1) * BLOCK]
        jump = first - last
        max_jump = max(max_jump, abs(jump))
        marker = " <<<" if abs(jump) > 500 else ""
        print(f"  Block {b:>2}→{b+1:>2}: {last:>+8.1f} → {first:>+8.1f}  Δ={jump:>+8.1f}{marker}")

    print(f"\n  Max block-boundary jump in simulation: {max_jump:.1f}")
    if max_jump < 500:
        print(f"  ✓ Simulation is SMOOTH — no block-boundary artifacts")
        print(f"  → Bug is NOT in the algorithm, must be in implementation")
    else:
        print(f"  ✗ Simulation has artifacts — bug is in the algorithm!")

    # FFT of simulation output
    sim_fft = np.abs(np.fft.rfft(sim_scaled[:2048]))
    sim_freqs = np.fft.rfftfreq(2048, 1.0 / FS)
    peak_idx = np.argmax(sim_fft[10:]) + 10  # skip DC
    print(f"\n  Simulated dominant frequency: {sim_freqs[peak_idx]:.1f} Hz (expected: 1000 Hz)")

    # Compare jumps: recording vs simulation
    print(f"\n{'='*60}")
    print(f"RECORDING vs SIMULATION COMPARISON")
    print(f"{'='*60}")
    rec_jumps = []
    for b in range(min(n_blocks - 1, 50)):
        last = seg[(b + 1) * BLOCK - 1]
        first = seg[(b + 1) * BLOCK]
        rec_jumps.append(abs(first - last))

    sim_jumps = []
    for b in range(min(n_sim_blocks - 1, 50)):
        last = sim_scaled[(b + 1) * BLOCK - 1]
        first = sim_scaled[(b + 1) * BLOCK]
        sim_jumps.append(abs(first - last))

    print(f"  Recording: mean block-boundary jump = {np.mean(rec_jumps):.1f}, max = {np.max(rec_jumps):.1f}")
    print(f"  Simulation: mean block-boundary jump = {np.mean(sim_jumps):.1f}, max = {np.max(sim_jumps):.1f}")

    if np.max(rec_jumps) > 10 * np.max(sim_jumps):
        print(f"\n  CONCLUSION: Recording has {np.max(rec_jumps)/np.max(sim_jumps):.0f}× larger jumps than simulation")
        print(f"  The algorithm is correct; the bug is in the IMPLEMENTATION.")
        print(f"  Possible causes:")
        print(f"    - DMA buffer reordering in I2S output")
        print(f"    - Race condition in ring buffer access")
        print(f"    - SPI frame loss causing ring buffer oscillation")
        print(f"    - Codec input bleeding into output buffer")


if __name__ == '__main__':
    main()
