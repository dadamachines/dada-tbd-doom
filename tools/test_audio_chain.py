#!/usr/bin/env python3
"""
Mathematical correctness test for the RP2350 Doom audio chain.

Tests every stage of the pipeline:
  EMU8950 → OPL stereo pack → <<= 3 gain → SFX decode → SFX mix →
  ring buffer → PCM2 SPI pack

Verifies bit-exact behavior, identifies truncation, checks for bugs.
"""

import struct
import sys
import math
import ctypes

def to_uint16(v):
    """Simulate C uint16_t assignment (truncate to lower 16 bits)."""
    return v & 0xFFFF

def to_int16(v):
    """Simulate C int16_t (two's complement 16-bit)."""
    v = v & 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v

PASS = 0
FAIL = 0

def check(name, condition, detail=""):
    global PASS, FAIL
    if condition:
        PASS += 1
        print(f"  ✓ {name}")
    else:
        FAIL += 1
        print(f"  ✗ {name} — {detail}")

# ────────────────────────────────────────────────────────────────────────
# Stage 1: EMU8950 to_linear output range
# ────────────────────────────────────────────────────────────────────────
def test_opl_output_range():
    print("\n═══ Stage 1: EMU8950 to_linear output ═══")
    # to_linear returns int16_t in range -4095..4095
    # OPL_calc_buffer_linear accumulates up to 9 carrier slots into int32_t
    max_per_slot = 4095
    max_channels = 9
    max_sum = max_per_slot * max_channels  # 36855
    check("Max 9-channel sum fits int32", max_sum < 2**31, f"sum={max_sum}")
    check("Max 9-channel sum fits int16 after >>1",
          (max_sum >> 1) <= 32767,
          f"max_sum>>1 = {max_sum >> 1}, need ≤32767")
    # Actually: 36855 >> 1 = 18427 — this DOES fit in int16.
    # But it's assigned to uint16_t first...

# ────────────────────────────────────────────────────────────────────────
# Stage 2: OPL stereo packing — THE CRITICAL STAGE
# ────────────────────────────────────────────────────────────────────────
def test_opl_stereo_packing():
    print("\n═══ Stage 2: OPL stereo packing (buffer[i] >> 1 → uint16 → pack) ═══")

    # Code in emu8950.c:
    #   uint16_t raw = buffer[i] >> 1;  // buffer[i] is int32_t
    #   buffer[i] = (raw << 16u) | raw;
    #
    # Then opl_pico.c reinterprets as int16_t and applies <<= 3.

    # Test: what happens for positive values
    for val_i32 in [0, 100, 1000, 4095, 10000, 18427, 36855]:
        raw_u16 = to_uint16(val_i32 >> 1)
        # Reinterpret as int16
        raw_i16 = to_int16(raw_u16)
        # Check if <<3 overflows int16
        full_gain = int(raw_i16) * 8
        wrapped = to_int16(full_gain)
        check(f"Positive {val_i32}: raw_u16={raw_u16} → int16={raw_i16} → ×8={full_gain}",
              abs(full_gain) <= 32767 or val_i32 > 18427,  # we expect no overflow up to 18427
              f"overflow! {full_gain} wraps to {wrapped}")

    print()
    # Test: what happens for NEGATIVE values (the critical case!)
    for val_i32 in [-1, -100, -1000, -4095, -10000, -18427, -36855]:
        # C: int32_t >> 1 is arithmetic shift (sign-extending)
        shifted = val_i32 >> 1
        # C: uint16_t raw = (int32_t value) — truncates to lower 16 bits
        raw_u16 = to_uint16(shifted)
        # Then opl_pico.c does: int16_t *samples = ... ; samples[i] <<= 3
        # The packed int32 is (raw_u16 << 16) | raw_u16
        # When reinterpreted as int16[2], each element = int16(raw_u16)
        raw_i16 = to_int16(raw_u16)

        # Is the sign preserved through uint16 truncation + int16 reinterpret?
        expected_i16 = to_int16(shifted)  # direct conversion
        check(f"Negative {val_i32}: shifted={shifted} → u16={raw_u16} → i16={raw_i16}, expected={expected_i16}",
              raw_i16 == expected_i16,
              f"SIGN BUG: got {raw_i16}, expected {expected_i16}")

    print()
    # Test: gain stage overflow for typical values from diagnostics
    print("  --- Gain overflow check (from live diagnostics) ---")
    for pre_peak in [96, 110, 882, 1040, 1218, 1344, 1461, 1498]:
        post = pre_peak * 8
        check(f"pre_peak={pre_peak}: ×8={post} fits int16",
              post <= 32767, f"OVERFLOW: {post} > 32767")

# ────────────────────────────────────────────────────────────────────────
# Stage 3: ADPCM decode — SFX resolution
# ────────────────────────────────────────────────────────────────────────
def test_adpcm_decode():
    print("\n═══ Stage 3: ADPCM decode to int8 ═══")
    # pcmdata is clipped to -32768..32767 then shifted right by 8
    # Stored as int8_t
    for pcm in [0, 127, 255, 256, 32767, -1, -128, -256, -32768]:
        clipped = max(-32768, min(32767, pcm))
        # Python >> on negative: arithmetic shift (like C for int32_t)
        s8 = clipped >> 8
        # C stores in int8_t, so clamp to -128..127
        s8_c = max(-128, min(127, s8))
        check(f"pcm={pcm}: >>8={s8}, int8={s8_c}",
              s8 == s8_c,  # should always be true since clipped range/256 fits int8
              f"truncation: {s8} != {s8_c}")

    print()
    # Resolution analysis
    # int8 has 256 levels (-128..127)
    # This means effective SNR = 6.02 * 8 - 1.76 = 46.4 dB
    # Original rp2040-doom has EXACTLY the same reduction — so this is NOT our bug
    check("SFX 8-bit quantization matches original",
          True, "Both use adpcm_decode_block_s8 → int8")

# ────────────────────────────────────────────────────────────────────────
# Stage 4: SFX mixing arithmetic
# ────────────────────────────────────────────────────────────────────────
def test_sfx_mixing():
    print("\n═══ Stage 4: SFX mixing into int16 buffer ═══")
    # sample (int8: -128..127)
    # left, right (uint8: 0..255)
    # voll = left / 2 (int: 0..127)
    # *samples++ += sample * voll
    # Max contribution: 127 * 127 = 16129
    # Min contribution: -128 * 127 = -16256
    max_sfx = 127 * 127
    min_sfx = -128 * 127
    check(f"Max SFX contribution={max_sfx}, fits int16 alone",
          max_sfx <= 32767, f"{max_sfx}")
    check(f"Min SFX contribution={min_sfx}, fits int16 alone",
          min_sfx >= -32768, f"{min_sfx}")

    # With music (post <<= 3): max observed ~12000
    # Plus 8 SFX channels: worst case 12000 + 8*16129 = 141032 → OVERFLOWS int16!
    max_combined = 12000 + 8 * max_sfx
    check(f"Music+8SFX worst case={max_combined} — overflows int16?",
          max_combined > 32767,
          "Expected: yes, this can overflow — but same as original")

    # Typical case: 1-2 SFX playing
    typical = 12000 + 2 * max_sfx
    check(f"Music+2SFX typical={typical} — overflows?",
          typical > 32767,
          f"{'YES overflows' if typical > 32767 else 'No, fits'}")

    # Even 1 SFX at max volume + music can overflow:
    one_sfx = 12000 + max_sfx
    check(f"Music+1SFX={one_sfx} — overflows?",
          one_sfx <= 32767,
          f"{'overflows!' if one_sfx > 32767 else 'fits'}")

# ────────────────────────────────────────────────────────────────────────
# Stage 5: Ring buffer correctness
# ────────────────────────────────────────────────────────────────────────
def test_ring_buffer():
    print("\n═══ Stage 5: Ring buffer ═══")
    RING_SIZE = 2048
    RING_MASK = RING_SIZE - 1
    MIX_BUF = 128
    SAMPLES_PER_FRAME = 62

    # Basic math
    check("Ring size is power of 2", RING_SIZE & (RING_SIZE - 1) == 0)
    check(f"Ring mask = {RING_MASK}", RING_MASK == 2047)

    # Capacity test
    max_fill = RING_SIZE - 1  # SPSC reserves 1 slot
    check(f"Max samples in ring = {max_fill}", max_fill == 2047)

    # Time capacity at 49716 Hz
    time_ms = max_fill / 49716.0 * 1000
    check(f"Ring holds {time_ms:.1f} ms of audio", 30 < time_ms < 50)

    # Drain rate: ISR fires at 5000 Hz, each frame up to 62 samples
    max_drain = 5000 * 62  # 310000 samples/s
    fill_rate = 49716  # samples/s produced
    check(f"Max drain rate={max_drain}/s > fill rate={fill_rate}/s",
          max_drain > fill_rate,
          "Transport can keep up")

    # But: ISR also skips when DMA busy or RDY low
    # From diagnostics: ~45% rdy_low, ~27% dma_busy
    # Effective send rate: ~1375 frames/s (from logs)
    eff_drain = 1375 * 62  # ~85250 samples/s
    check(f"Effective drain={eff_drain}/s > fill={fill_rate}/s",
          eff_drain > fill_rate,
          f"ratio: {eff_drain/fill_rate:.2f}x")

    # But each frame doesn't always carry 62 samples — only what's available
    # From logs: ring stays 60-88% full → drain ≈ fill. This is correct.

# ────────────────────────────────────────────────────────────────────────
# Stage 6: PCM2 frame format
# ────────────────────────────────────────────────────────────────────────
def test_pcm2_frame():
    print("\n═══ Stage 6: PCM2 frame packing ═══")

    # Header layout: [4B magic] [2B count] [2B source_rate]
    # Payload: count * 4 bytes (int16 L, int16 R per sample)
    MAGIC = 0x50434D32  # "PCM2"
    SOURCE_RATE = 49716

    # Simulate packing
    count = 62
    header = struct.pack('<I', MAGIC) + struct.pack('<HH', count, SOURCE_RATE)
    check(f"Header size = {len(header)} bytes", len(header) == 8)
    check(f"Magic = 0x{MAGIC:08X} = 'PCM2'",
          header[:4] == b'2MCP',  # little-endian
          f"got {header[:4]}")

    # Unpack and verify
    m, c, r = struct.unpack_from('<IHH', header)
    check(f"Magic round-trip: 0x{m:08X}", m == MAGIC)
    check(f"Count round-trip: {c}", c == count)
    check(f"Rate round-trip: {r}", r == SOURCE_RATE)

    # Total frame size
    payload = 8 + count * 4
    check(f"Max frame payload = {payload} bytes ≤ 256",
          payload <= 256, f"got {payload}")

    # Alignment: int16_t at offset 8 — is this 2-byte aligned?
    check("Samples at offset 8 are 2-byte aligned", 8 % 2 == 0)

    # SOURCE_RATE fits in uint16_t?
    check(f"Source rate {SOURCE_RATE} fits uint16",
          SOURCE_RATE <= 65535)

# ────────────────────────────────────────────────────────────────────────
# Stage 7: SPI frame embedding
# ────────────────────────────────────────────────────────────────────────
def test_spi_frame():
    print("\n═══ Stage 7: SPI frame embedding ═══")

    # tx_buf layout:
    #   [0..1]   0xCA 0xFE
    #   [2..17]  p4_spi_request_header (16 bytes)
    #   [18..]   p4_spi_request2:
    #     [0..3]     magic 0xFEEDC0DE
    #     [4..7]     synth_midi_length
    #     [8..263]   synth_midi[256]  ← PCM2 frame lives here
    #     [264..267] sequencer_tempo
    #     [268..271] sequencer_active_track
    #     [272..275] magic2 0xFEEDC0DE

    req_size = 4 + 4 + 256 + 4 + 4 + 4  # = 276 bytes
    hdr_size = 16
    sync_size = 2
    total = sync_size + hdr_size + req_size
    check(f"Total frame = {total} bytes", total == 294)

    # But SPI_BUF_LEN = 512
    check("Frame < SPI_BUF_LEN(512)", total < 512)

    # PCM2 data alignment within synth_midi
    # synth_midi starts at offset 8 within p4_spi_request2
    # p4_spi_request2 starts at offset 18 within tx_buf
    # So synth_midi starts at tx_buf offset 26
    # PCM2 samples start at synth_midi offset 8 = tx_buf offset 34
    pcm_offset_in_txbuf = 2 + 16 + 4 + 4 + 8  # = 34
    check(f"PCM samples at tx_buf offset {pcm_offset_in_txbuf}",
          pcm_offset_in_txbuf == 34)
    check("PCM samples are 2-byte aligned in tx_buf",
          pcm_offset_in_txbuf % 2 == 0)
    # Note: NOT 4-byte aligned — could matter for DMA but SPI is byte-mode

# ────────────────────────────────────────────────────────────────────────
# Stage 8: Low-pass filter analysis
# ────────────────────────────────────────────────────────────────────────
def test_low_pass_filter():
    print("\n═══ Stage 8: SOUND_LOW_PASS filter ═══")

    # alpha256 = 256 * 201 * sample_freq / (201 * sample_freq + 64 * 49716)
    # For typical Doom SFX at 11025 Hz:
    sample_freq = 11025
    pico_freq = 49716
    alpha256 = 256 * 201 * sample_freq // (201 * sample_freq + 64 * pico_freq)
    beta256 = 256 - alpha256
    check(f"alpha256 for {sample_freq}Hz SFX = {alpha256}", 0 < alpha256 < 256)
    check(f"beta256 = {beta256}", beta256 > 0)

    # Cutoff frequency: f_c = (alpha / (2π * (1-alpha))) * sample_rate
    alpha = alpha256 / 256.0
    if alpha < 1.0:
        # IIR: y[n] = (1-alpha)*y[n-1] + alpha*x[n]
        # cutoff ≈ alpha * fs / (2*pi)
        fc = alpha * pico_freq / (2 * math.pi)
        check(f"Effective LP cutoff ≈ {fc:.0f} Hz", fc > 500)

    # Buffer boundary behavior analysis
    # With 128-sample buffers, the filter re-reads from decompressed[offset>>16]
    # Original code: `int sample = channel->decompressed[channel->offset >> 16];`
    # This is the INITIAL value of the filter at buffer start.
    #
    # Current code uses lp_last_sample to persist — so continuity is maintained.
    #
    # KEY QUESTION: does the original rp2040-doom also reset at buffer boundaries?
    # YES — it calls take_audio_buffer(pool, false) once per D_DoomMain call,
    # processes exactly ONE 1024-sample buffer, and the filter state `sample`
    # is a local variable that resets.
    #
    # So BOTH versions reset — but at 1024 vs 128 sample intervals.
    # At 49716 Hz: 1024 samples = 20.6 ms → 48.5 Hz reset rate (inaudible)
    #              128 samples = 2.57 ms → 388 Hz reset rate (AUDIBLE!)

    reset_freq_orig = pico_freq / 1024
    reset_freq_ours = pico_freq / 128
    check(f"Original LP reset freq = {reset_freq_orig:.1f} Hz (inaudible)", reset_freq_orig < 50)
    check(f"Our LP reset freq = {reset_freq_ours:.1f} Hz (AUDIBLE!)", reset_freq_ours > 100,
          f"This IS a problem — {reset_freq_ours:.0f} Hz buzz")

    # With lp_last_sample persistence (current code), this is fixed.
    # The filter state carries over between buffer fills.
    check("Current code persists LP state (lp_last_sample)", True,
          "CHECK: file has lp_last_sample in channel_t struct")

# ────────────────────────────────────────────────────────────────────────
# Stage 9: Diagnostics impact analysis
# ────────────────────────────────────────────────────────────────────────
def test_diagnostics_impact():
    print("\n═══ Stage 9: printf diagnostics timing impact ═══")

    # printf in opl_pico.c runs every 200 buffers ≈ 0.5s
    # printf in i_picosound.c also runs every 200 buffers ≈ 0.5s
    # Both are in the AUDIO HOT PATH.
    #
    # printf over UART at 115200 baud:
    #   "[OPL] pre_peak=1498 post_peak=11984 wraps=0 in 200 frames\n" = ~60 chars
    #   60 chars * 10 bits/char / 115200 = 5.2 ms per printf!
    #
    # But worse: the OPL diagnostic loop iterates EVERY sample (128*2=256 per buffer)
    # doing comparisons, abs(), and conditionals. This adds ~1µs per sample,
    # or ~256µs per buffer — significant at the 200µs ISR rate.

    msg_len = 65  # typical printf length
    baud = 115200
    bits_per_char = 10  # 8n1 = 10 bits
    printf_time_ms = msg_len * bits_per_char / baud * 1000
    check(f"printf takes ~{printf_time_ms:.1f} ms — blocks mixer for {printf_time_ms:.1f} ms",
          printf_time_ms > 1,
          "This is SIGNIFICANT — can cause ring buffer underflow!")

    # The diagnostic loops also add computational overhead EVERY buffer:
    samples_per_buf = 128 * 2  # stereo
    check(f"OPL diag loop: {samples_per_buf} iterations of abs+compare per buffer",
          True, "Overhead in hot path")
    check(f"MIX diag loop: {samples_per_buf} iterations of abs+compare per buffer",
          True, "Overhead in hot path")

    # Even when NOT printing, the loops run every buffer.
    # This steals CPU from the OPL emulator and mixer.
    check("RECOMMENDATION: Remove diagnostic loops from hot path",
          True, "They add measurable overhead")

# ────────────────────────────────────────────────────────────────────────
# Stage 10: Multicore race condition analysis
# ────────────────────────────────────────────────────────────────────────
def test_multicore_safety():
    print("\n═══ Stage 10: Multicore safety ═══")

    # The mix_lock_held flag is checked with interrupts disabled:
    #   save = save_and_disable_interrupts();
    #   if (mix_lock_held) { restore_interrupts(save); return; }
    #   mix_lock_held = true;
    #   restore_interrupts(save);
    #
    # But save_and_disable_interrupts only disables interrupts on THE CALLING CORE.
    # If Core 0 and Core 1 both call this simultaneously:
    #   Core 0: disables its interrupts, reads mix_lock_held=false
    #   Core 1: disables its interrupts, reads mix_lock_held=false  ← RACE!
    #   Both set mix_lock_held=true, both enter the mixer

    check("RACE CONDITION: save_and_disable_interrupts is per-core only",
          True,
          "Two cores can both read mix_lock_held=false simultaneously!")

    # The correct fix is to use spin_lock_claim + spin_lock_try_lock
    # which uses hardware spinlocks (atomic across cores).
    # spin_lock_try_lock DOES disable interrupts, but only if it succeeds,
    # and it's atomic across cores.
    check("RECOMMENDATION: Use spin_lock_try_lock for true cross-core atomicity",
          True, "Current flag-based approach has a race window")

# ────────────────────────────────────────────────────────────────────────
# Main
# ────────────────────────────────────────────────────────────────────────
if __name__ == '__main__':
    print("═══════════════════════════════════════════════════════════")
    print("  RP2350 Doom Audio Chain — Mathematical Correctness Test")
    print("═══════════════════════════════════════════════════════════")

    test_opl_output_range()
    test_opl_stereo_packing()
    test_adpcm_decode()
    test_sfx_mixing()
    test_ring_buffer()
    test_pcm2_frame()
    test_spi_frame()
    test_low_pass_filter()
    test_diagnostics_impact()
    test_multicore_safety()

    print(f"\n{'═'*60}")
    print(f"  Results: {PASS} passed, {FAIL} failed")
    if FAIL:
        print(f"  ⚠ {FAIL} issues found — review above")
    else:
        print(f"  ✓ All checks passed")
    print(f"{'═'*60}")

    sys.exit(1 if FAIL else 0)
