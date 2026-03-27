#!/usr/bin/env python3
"""Analyze Doom audio recording from P4 codec output.
   Generates spectral, clipping, dynamic range, and transient analysis."""

import sys
import wave
import struct
import numpy as np
from scipy import signal
from scipy.fft import rfft, rfftfreq
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

def load_wav(path):
    with wave.open(path, 'rb') as wf:
        nch = wf.getnchannels()
        sw = wf.getsampwidth()
        sr = wf.getframerate()
        n = wf.getnframes()
        raw = wf.readframes(n)
    if sw == 2:
        fmt = f'<{n * nch}h'
        samples = np.array(struct.unpack(fmt, raw), dtype=np.float64)
    else:
        raise ValueError(f"Unsupported sample width: {sw}")
    samples = samples.reshape(-1, nch)
    return samples, sr, nch

def analyze(wav_path, out_dir):
    print(f"Loading {wav_path}...")
    samples, sr, nch = load_wav(wav_path)
    duration = len(samples) / sr
    print(f"  Sample rate: {sr} Hz")
    print(f"  Channels: {nch}")
    print(f"  Duration: {duration:.2f}s")
    print(f"  Total samples: {len(samples)}")

    # Use mono mix for most analysis
    if nch == 2:
        mono = (samples[:, 0] + samples[:, 1]) / 2.0
        left = samples[:, 0]
        right = samples[:, 1]
    else:
        mono = samples[:, 0]
        left = right = mono

    # Normalize to [-1, 1] for analysis
    mono_norm = mono / 32768.0
    left_norm = left / 32768.0
    right_norm = right / 32768.0

    print("\n" + "="*60)
    print("1. LEVEL & CLIPPING ANALYSIS")
    print("="*60)
    peak_l = np.max(np.abs(left))
    peak_r = np.max(np.abs(right))
    peak_mono = np.max(np.abs(mono))
    rms_mono = np.sqrt(np.mean(mono**2))
    print(f"  Peak L: {peak_l:.0f} / 32767 ({20*np.log10(peak_l/32767):.1f} dBFS)")
    print(f"  Peak R: {peak_r:.0f} / 32767 ({20*np.log10(peak_r/32767):.1f} dBFS)")
    print(f"  Peak mono: {peak_mono:.0f} / 32767 ({20*np.log10(peak_mono/32767):.1f} dBFS)")
    print(f"  RMS mono: {rms_mono:.0f} ({20*np.log10(rms_mono/32767):.1f} dBFS)")

    # Clipping detection: samples at exactly +/- 32767 or 32768
    clip_l = np.sum(np.abs(left) >= 32767)
    clip_r = np.sum(np.abs(right) >= 32767)
    print(f"  Clipped samples L: {clip_l} ({100*clip_l/len(left):.4f}%)")
    print(f"  Clipped samples R: {clip_r} ({100*clip_r/len(right):.4f}%)")

    # Near-clipping (within 1 dB of full scale)
    near_clip_thresh = 32767 * 10**(-1/20)  # -1 dBFS
    near_l = np.sum(np.abs(left) >= near_clip_thresh)
    near_r = np.sum(np.abs(right) >= near_clip_thresh)
    print(f"  Near-clip (>-1dBFS) L: {near_l} ({100*near_l/len(left):.4f}%)")
    print(f"  Near-clip (>-1dBFS) R: {near_r} ({100*near_r/len(right):.4f}%)")

    # Crest factor
    crest = peak_mono / rms_mono if rms_mono > 0 else 0
    print(f"  Crest factor: {20*np.log10(crest):.1f} dB")

    print("\n" + "="*60)
    print("2. SILENCE & DYNAMIC RANGE ANALYSIS")
    print("="*60)
    # Analyze in 10ms windows
    win_samples = int(sr * 0.010)
    n_windows = len(mono) // win_samples
    rms_windows = np.array([
        np.sqrt(np.mean(mono[i*win_samples:(i+1)*win_samples]**2))
        for i in range(n_windows)
    ])
    rms_db = 20 * np.log10(rms_windows / 32767 + 1e-10)

    silence_thresh_db = -60
    n_silent = np.sum(rms_db < silence_thresh_db)
    print(f"  Silent windows (<{silence_thresh_db}dBFS): {n_silent}/{n_windows} ({100*n_silent/n_windows:.1f}%)")
    print(f"  RMS range: {np.min(rms_db):.1f} to {np.max(rms_db):.1f} dBFS")
    print(f"  Median RMS: {np.median(rms_db):.1f} dBFS")

    # Check for DC offset
    dc_offset = np.mean(mono)
    print(f"  DC offset: {dc_offset:.2f} ({dc_offset/32767*100:.3f}%)")

    print("\n" + "="*60)
    print("3. SPECTRAL ANALYSIS")
    print("="*60)
    # Full FFT
    N = len(mono)
    freqs = rfftfreq(N, 1.0/sr)
    fft_mag = np.abs(rfft(mono_norm * np.hanning(N)))
    fft_db = 20 * np.log10(fft_mag + 1e-10)

    # Find dominant frequencies
    # Average spectrum in 1-second chunks for stability
    chunk = sr
    n_chunks = N // chunk
    avg_spec = np.zeros(chunk // 2 + 1)
    for i in range(n_chunks):
        seg = mono_norm[i*chunk:(i+1)*chunk]
        spec = np.abs(rfft(seg * np.hanning(chunk)))
        avg_spec += spec
    avg_spec /= n_chunks
    avg_freqs = rfftfreq(chunk, 1.0/sr)
    avg_db = 20 * np.log10(avg_spec + 1e-10)

    # Energy in frequency bands
    bands = [(20, 200, "Sub-bass/Bass"), (200, 1000, "Low-mid"),
             (1000, 4000, "Mid"), (4000, 8000, "Upper-mid"),
             (8000, 16000, "Presence"), (16000, 22050, "Air/Brilliance")]
    print("  Frequency band energy (dB relative to peak):")
    peak_band_energy = -999
    band_energies = {}
    for lo, hi, name in bands:
        mask = (avg_freqs >= lo) & (avg_freqs < hi)
        if np.any(mask):
            energy = np.sqrt(np.mean(avg_spec[mask]**2))
            energy_db = 20 * np.log10(energy + 1e-10)
            band_energies[name] = energy_db
            if energy_db > peak_band_energy:
                peak_band_energy = energy_db

    for name, db in band_energies.items():
        rel = db - peak_band_energy
        bar = "#" * max(0, int((rel + 60) / 2))
        print(f"    {name:20s}: {rel:+6.1f} dB  {bar}")

    # Check for aliasing: energy above Nyquist/2 of source (49716/2 = 24858)
    # Since we're at 44100, Nyquist is 22050. The alias of 24858 Hz folds to 44100-24858=19242 Hz
    # Look for suspicious energy around the alias fold frequency
    alias_freq = 44100 - (49716 / 2)  # = 19242 Hz (where 24858 would fold)
    alias_band = (avg_freqs >= alias_freq - 500) & (avg_freqs <= alias_freq + 500)
    nearby_band = (avg_freqs >= alias_freq - 2000) & (avg_freqs <= alias_freq - 500)
    if np.any(alias_band) and np.any(nearby_band):
        alias_energy = 20 * np.log10(np.sqrt(np.mean(avg_spec[alias_band]**2)) + 1e-10)
        nearby_energy = 20 * np.log10(np.sqrt(np.mean(avg_spec[nearby_band]**2)) + 1e-10)
        print(f"\n  Aliasing check (49716->44100 fold at ~{alias_freq:.0f} Hz):")
        print(f"    Energy at alias freq: {alias_energy:.1f} dB")
        print(f"    Energy nearby (lower): {nearby_energy:.1f} dB")
        print(f"    Difference: {alias_energy - nearby_energy:+.1f} dB {'⚠ POSSIBLE ALIASING' if alias_energy > nearby_energy + 3 else '✓ OK'}")

    # Check for 49716 Hz resampling artifacts - look for periodic spectral notches/peaks
    print(f"\n  Spectral roll-off:")
    for f in [5000, 10000, 15000, 18000, 20000, 21000]:
        mask = (avg_freqs >= f-100) & (avg_freqs <= f+100)
        if np.any(mask):
            e = 20 * np.log10(np.sqrt(np.mean(avg_spec[mask]**2)) + 1e-10)
            print(f"    {f:6d} Hz: {e - peak_band_energy:+.1f} dB")

    print("\n" + "="*60)
    print("4. DISTORTION ANALYSIS")
    print("="*60)
    # THD estimation - find strongest tone and measure harmonics
    # Use a segment with clear tonal content (music)
    # Take 2 second chunks and find the one with most tonal content
    chunk_2s = 2 * sr
    best_thd_chunk = None
    best_peak_freq = 0
    best_peak_power = -999
    for i in range(N // chunk_2s):
        seg = mono_norm[i*chunk_2s:(i+1)*chunk_2s]
        spec = np.abs(rfft(seg * np.hanning(len(seg))))
        spec_db = 20 * np.log10(spec + 1e-10)
        # Find peak between 50-5000 Hz (music range)
        freq_2s = rfftfreq(len(seg), 1.0/sr)
        music_mask = (freq_2s >= 50) & (freq_2s <= 5000)
        if np.any(music_mask):
            peak_idx = np.argmax(spec[music_mask]) + np.argmax(music_mask)
            if spec_db[peak_idx] > best_peak_power:
                best_peak_power = spec_db[peak_idx]
                best_peak_freq = freq_2s[peak_idx]
                best_thd_chunk = i

    if best_thd_chunk is not None and best_peak_freq > 0:
        seg = mono_norm[best_thd_chunk*chunk_2s:(best_thd_chunk+1)*chunk_2s]
        spec = np.abs(rfft(seg * np.hanning(len(seg))))
        freq_2s = rfftfreq(len(seg), 1.0/sr)
        fundamental_power = 0
        harmonic_power = 0
        f0 = best_peak_freq
        for h in range(1, 8):
            hf = f0 * h
            if hf > sr/2:
                break
            mask = (freq_2s >= hf*0.98) & (freq_2s <= hf*1.02)
            if np.any(mask):
                power = np.sum(spec[mask]**2)
                if h == 1:
                    fundamental_power = power
                else:
                    harmonic_power += power
        if fundamental_power > 0:
            thd = np.sqrt(harmonic_power / fundamental_power) * 100
            print(f"  Strongest tone: {f0:.1f} Hz at {best_peak_power:.1f} dB")
            print(f"  THD estimate: {thd:.2f}%")
            if thd > 5:
                print(f"  ⚠ THD > 5% — significant harmonic distortion")
            elif thd > 1:
                print(f"  ⚠ THD > 1% — mild harmonic distortion")
            else:
                print(f"  ✓ THD < 1% — low distortion")

    # Intermodulation distortion: check for unexpected sum/difference frequencies
    # Look for buzzy artifacts in 100-500 Hz range that shouldn't be there
    noise_floor_band = (avg_freqs >= 15000) & (avg_freqs <= 20000)
    signal_band = (avg_freqs >= 100) & (avg_freqs <= 5000)
    if np.any(noise_floor_band) and np.any(signal_band):
        nf = 20 * np.log10(np.sqrt(np.mean(avg_spec[noise_floor_band]**2)) + 1e-10)
        sg = 20 * np.log10(np.sqrt(np.mean(avg_spec[signal_band]**2)) + 1e-10)
        snr = sg - nf
        print(f"\n  SNR estimate (100-5kHz vs 15-20kHz): {snr:.1f} dB")

    print("\n" + "="*60)
    print("5. RESAMPLING ARTIFACT ANALYSIS")
    print("="*60)
    # Check for periodic clicking/popping (ring buffer underruns)
    # Compute sample-to-sample differences
    diff = np.diff(mono_norm)
    diff_abs = np.abs(diff)

    # Threshold for "click" detection: sudden jumps > 10x local RMS
    local_rms_win = 512
    n_diff = len(diff)
    click_count = 0
    click_positions = []
    for i in range(local_rms_win, n_diff - local_rms_win, local_rms_win):
        local_rms = np.sqrt(np.mean(diff[i-local_rms_win:i+local_rms_win]**2))
        if local_rms > 0:
            chunk_max = np.max(diff_abs[i:i+local_rms_win])
            if chunk_max > local_rms * 15:
                click_count += 1
                click_pos = i + np.argmax(diff_abs[i:i+local_rms_win])
                click_positions.append(click_pos / sr)

    print(f"  Detected clicks/pops: {click_count}")
    if click_count > 0:
        print(f"  Click rate: {click_count / duration:.2f} per second")
        if len(click_positions) > 1:
            intervals = np.diff(click_positions)
            print(f"  Average interval: {np.mean(intervals)*1000:.1f} ms")
            print(f"  First 10 click times: {[f'{t:.3f}s' for t in click_positions[:10]]}")
            # Check if clicks are periodic (buffer underrun pattern)
            if len(intervals) > 3:
                cv = np.std(intervals) / np.mean(intervals) if np.mean(intervals) > 0 else 999
                if cv < 0.3:
                    print(f"  ⚠ Clicks appear PERIODIC (CV={cv:.2f}) — likely buffer underruns")
                else:
                    print(f"  Clicks appear random (CV={cv:.2f})")

    # Check stereo correlation (should be high for mono-sourced Doom audio)
    if nch == 2:
        corr = np.corrcoef(left_norm, right_norm)[0, 1]
        print(f"\n  Stereo correlation: {corr:.4f}")
        if corr > 0.999:
            print(f"  ✓ Essentially mono (as expected for Doom)")
        elif corr > 0.99:
            print(f"  ⚠ Slight stereo difference — possible channel delay or processing")
        else:
            print(f"  ⚠ Significant stereo difference — unexpected for Doom")

    # Zero-crossing rate analysis (can reveal quantization noise)
    zc = np.sum(np.diff(np.sign(mono_norm)) != 0)
    zcr = zc / duration
    print(f"\n  Zero-crossing rate: {zcr:.0f} Hz")

    print("\n" + "="*60)
    print("6. SPECTROGRAM GENERATION")
    print("="*60)

    fig, axes = plt.subplots(5, 1, figsize=(16, 20))

    # 6a. Waveform
    t = np.arange(len(mono)) / sr
    axes[0].plot(t, mono_norm, linewidth=0.1, color='#2196F3')
    axes[0].set_title('Waveform', fontsize=14)
    axes[0].set_xlabel('Time (s)')
    axes[0].set_ylabel('Amplitude')
    axes[0].set_ylim(-1.1, 1.1)
    axes[0].axhline(y=1.0, color='red', linewidth=0.5, linestyle='--', alpha=0.5)
    axes[0].axhline(y=-1.0, color='red', linewidth=0.5, linestyle='--', alpha=0.5)

    # 6b. Spectrogram (full range)
    f_spec, t_spec, Sxx = signal.spectrogram(mono_norm, sr, nperseg=2048, noverlap=1536)
    Sxx_db = 10 * np.log10(Sxx + 1e-10)
    im = axes[1].pcolormesh(t_spec, f_spec, Sxx_db, shading='gouraud', cmap='magma', vmin=-80, vmax=0)
    axes[1].set_title('Spectrogram (0-22 kHz)', fontsize=14)
    axes[1].set_ylabel('Frequency (Hz)')
    axes[1].set_xlabel('Time (s)')
    axes[1].set_ylim(0, 22050)
    plt.colorbar(im, ax=axes[1], label='dB')

    # 6c. Spectrogram (zoomed 0-8kHz for detail)
    im2 = axes[2].pcolormesh(t_spec, f_spec, Sxx_db, shading='gouraud', cmap='magma', vmin=-80, vmax=0)
    axes[2].set_title('Spectrogram (0-8 kHz detail)', fontsize=14)
    axes[2].set_ylabel('Frequency (Hz)')
    axes[2].set_xlabel('Time (s)')
    axes[2].set_ylim(0, 8000)
    plt.colorbar(im2, ax=axes[2], label='dB')

    # 6d. Average spectrum
    axes[3].plot(avg_freqs, avg_db, linewidth=0.5, color='#4CAF50')
    axes[3].set_title('Average Spectrum', fontsize=14)
    axes[3].set_xlabel('Frequency (Hz)')
    axes[3].set_ylabel('Magnitude (dB)')
    axes[3].set_xlim(20, 22050)
    axes[3].set_xscale('log')
    axes[3].grid(True, alpha=0.3)
    # Mark key frequencies
    axes[3].axvline(x=19242, color='red', linewidth=1, linestyle='--', alpha=0.7, label='Alias fold (19.2kHz)')
    axes[3].axvline(x=20000, color='orange', linewidth=1, linestyle='--', alpha=0.7, label='AA filter (20kHz)')
    axes[3].legend(fontsize=9)

    # 6e. RMS over time
    t_rms = np.arange(n_windows) * 0.010
    axes[4].plot(t_rms, rms_db, linewidth=0.5, color='#FF5722')
    axes[4].set_title('RMS Level Over Time (10ms windows)', fontsize=14)
    axes[4].set_xlabel('Time (s)')
    axes[4].set_ylabel('RMS (dBFS)')
    axes[4].set_ylim(-80, 0)
    axes[4].grid(True, alpha=0.3)

    plt.tight_layout()
    plot_path = f"{out_dir}/audio_analysis.png"
    plt.savefig(plot_path, dpi=150)
    print(f"  Saved: {plot_path}")

    # Additional plot: high-frequency detail
    fig2, axes2 = plt.subplots(2, 1, figsize=(16, 8))

    # Zoom into 15-22 kHz to see AA filter effect and aliasing
    hf_mask = (avg_freqs >= 10000) & (avg_freqs <= 22050)
    axes2[0].plot(avg_freqs[hf_mask], avg_db[hf_mask], linewidth=0.8, color='#9C27B0')
    axes2[0].set_title('High Frequency Detail (10-22 kHz) — AA Filter & Aliasing Check', fontsize=14)
    axes2[0].set_xlabel('Frequency (Hz)')
    axes2[0].set_ylabel('Magnitude (dB)')
    axes2[0].grid(True, alpha=0.3)
    axes2[0].axvline(x=19242, color='red', linewidth=1, linestyle='--', label='Alias fold 19.2kHz')
    axes2[0].axvline(x=20000, color='orange', linewidth=1, linestyle='--', label='AA cutoff 20kHz')
    axes2[0].legend()

    # Transient detail: plot a 50ms segment around loudest click (if any)
    if click_positions:
        click_sample = int(click_positions[0] * sr)
        win = int(0.025 * sr)
        start = max(0, click_sample - win)
        end = min(len(mono_norm), click_sample + win)
        t_click = np.arange(start, end) / sr * 1000
        axes2[1].plot(t_click, mono_norm[start:end], linewidth=0.5, color='#F44336')
        axes2[1].set_title(f'Click/Pop Detail at t={click_positions[0]:.3f}s', fontsize=14)
        axes2[1].set_xlabel('Time (ms)')
        axes2[1].set_ylabel('Amplitude')
        axes2[1].grid(True, alpha=0.3)
    else:
        # Show a transient instead - find loudest moment
        peak_idx = np.argmax(np.abs(mono_norm))
        win = int(0.025 * sr)
        start = max(0, peak_idx - win)
        end = min(len(mono_norm), peak_idx + win)
        t_peak = np.arange(start, end) / sr * 1000
        axes2[1].plot(t_peak, mono_norm[start:end], linewidth=0.5, color='#2196F3')
        axes2[1].set_title(f'Loudest Transient at t={peak_idx/sr:.3f}s', fontsize=14)
        axes2[1].set_xlabel('Time (ms)')
        axes2[1].set_ylabel('Amplitude')
        axes2[1].grid(True, alpha=0.3)

    plt.tight_layout()
    plot_path2 = f"{out_dir}/audio_analysis_hf.png"
    plt.savefig(plot_path2, dpi=150)
    print(f"  Saved: {plot_path2}")

    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)

if __name__ == "__main__":
    wav_path = sys.argv[1] if len(sys.argv) > 1 else "doomaudio.wav"
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "tools"
    analyze(wav_path, out_dir)
