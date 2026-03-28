# Temporal Dithering Design Review — SSD1309 on RP2350

Architecture review for improving grayscale simulation quality on the TBD-16 DOOM port.
Grounded in the actual codebase, the UG-2864AxxPG01 OLED module datasheet, and the upstream rp2040-doom temporal greyscale implementation.

**Status:** All previous dithering tests (modes 0–14) were conducted *before* the switch to PIO SPI. The PIO driver provides significantly more consistent transfer timing than the previous path. Every mode should be re-evaluated with PIO SPI in place.

**Core problem:** The game is playable but the perception is that resolution isn't sufficient. This is a dithering quality issue, not a pixel count issue — the 128×64 physical resolution cannot change, but perceived resolution is dominated by how well the dithering converts 8-bit luminance into 1-bit decisions.

---

## 1. Hardware Constraints from Datasheet

Source: UG-2864ASWPG01 Product Specification (Topwin Semiconductor), SSD1309 controller.

### Display Physical Parameters

| Parameter | Value | Implication |
|-----------|-------|-------------|
| Active area | 55.01 × 27.49 mm | 2.42" diagonal |
| Pixel pitch | 0.43 × 0.43 mm | Each pixel is ~430 µm — clearly visible at arm's length |
| Pixel size | 0.40 × 0.40 mm | 0.03 mm gap between pixels — visible grid |
| Resolution | 128 × 64 | 8192 total pixels |
| Display mode | Passive matrix, 1/64 duty | Each row is driven 1/64th of the time |
| Drive duty | 1/64 | All 64 COM lines multiplexed |

### Timing Parameters (Serial / SPI)

From datasheet Section 3.3.3:

| Parameter | Min | Max | Unit |
|-----------|-----|-----|------|
| Clock cycle time (t_cycle) | 100 | — | ns |
| Clock low time (t_CLKL) | 50 | — | ns |
| Clock high time (t_CLKH) | 50 | — | ns |
| Data setup time (t_DSW) | 15 | — | ns |
| Data hold time (t_DHW) | 15 | — | ns |
| CS setup time (t_CSS) | 20 | — | ns |
| CS hold time (t_CSH) | 50 | — | ns |

**Maximum SPI clock = 1 / 100 ns = 10 MHz.** We run at the datasheet maximum.

### Initialization Differences: Datasheet vs Our Code

| Command | Datasheet Example | Our Init | Notes |
|---------|------------------|----------|-------|
| Oscillator (0xD5) | **0xA0** | **0xF0** | We use max oscillator — faster internal scan |
| Contrast (0x81) | **0xDF** | **0xCF** | We're 16 steps lower. Headroom available. |
| COM scan (0xC8) | **0xC8** (remapped) | **0xC0** (normal) | We use software Y-flip instead |
| Segment remap | 0xA1 | 0xA1 | Match — both mirror for 180° mounting |
| Pre-charge (0xD9) | 0x82 | 0x82 | Match |
| VCOMH (0xDB) | 0x34 | 0x34 | Match |

**Key finding:** The datasheet example uses **0xC8** (COM scan remapped) for upside-down mounting, while we use **0xC0** + software Y-flip. This is functionally equivalent but the software flip costs CPU cycles in every dithering mode. Switching to 0xC8 (hardware vertical flip) could eliminate the software Y-flip entirely — saving one subtraction per pixel (8192 operations per frame).

**Potential contrast boost:** We run contrast at 0xCF but the datasheet example uses 0xDF. Since we're not doing temporal contrast modulation (except in mode 3), raising contrast to 0xDF could improve brightness and perceived quality for all spatial dithering modes.

### Internal Scan Rate Estimate

The SSD1309 internal frame rate depends on the oscillator frequency command (0xD5):

```
f_frame = f_osc / (D × K × MUX)
```

Where:
- f_osc = oscillator frequency (max at 0xD5 upper nibble = 0xF, ~407 kHz per SSD1306 datasheet)
- D = clock divide ratio (lower nibble 0x0 = divide by 1)
- K = phase periods per multiplex step (~54 for our pre-charge settings)
- MUX = 64 (all COM lines active)

```
f_frame ≈ 407000 / (1 × 54 × 64) ≈ 118 Hz
```

**The SSD1309 internally refreshes at approximately 118 Hz.** This is the rate at which it scans through all 64 rows of GRAM. There is no VSYNC output — we cannot synchronize writes to this scan.

### OLED Lifetime Consideration

From datasheet Section 2:
- At 80 cd/m²: 30,000 hours
- At 60 cd/m²: 50,000 hours

Temporal dithering with contrast modulation (mode 3 style) varies current draw per sub-frame, which may be gentler on the OLED than constant high-contrast. But at our pixel sizes this is not a practical concern for a game device.

---

## 2. Current PIO SPI Transfer Characteristics

Since the switch to PIO SPI, the display transfer path is fundamentally different from what was tested with previous dithering modes:

### PIO SPI Specifications (current)

| Parameter | Value |
|-----------|-------|
| PIO | pio0, SM 0 |
| Clock | 10 MHz (150 MHz sys / 2 cycles per bit / clkdiv 7.5) |
| Protocol | SPI Mode 0 (CPOL=0, CPHA=0) |
| Shift | MSB first, autopull 8-bit |
| SCK | GPIO 14 (side-set) |
| MOSI | GPIO 15 (OUT) |
| Program | 2 instructions: `out pins, 1 side 0` / `nop side 1` |

### Transfer Timing

```
1 byte  = 8 bits × 2 PIO cycles × (1/10 MHz) = 1.6 µs per byte
6 bytes (command_park)                        = 9.6 µs
1024 bytes (frame data)                       = 1638 µs ≈ 1.6 ms
DC toggle + CS assertions                     ≈ 0.01 ms
                                     Total    ≈ 1.65 ms per frame write
```

At 10 MHz PIO clock, each byte takes 8 bits × 2 PIO cycles × (1/10 MHz) = 1.6 µs. With DMA, FIFO stall overhead is eliminated.

### Why PIO SPI Matters for Temporal Dithering

The previous SPI path (hardware SPI1 or bit-banged) had variable transfer timing due to:
- Interrupt latency on the SPI peripheral
- Variable FIFO drain timing
- Potential contention with other SPI users

The PIO SPI provides:
- **Deterministic bit timing** — the 2-instruction PIO program runs at a fixed rate regardless of CPU activity
- **No interrupt jitter** — PIO runs independently of ARM cores
- **Consistent frame-to-frame transfer duration** — critical for temporal dithering where cadence stability determines rolling-bar visibility

**This means temporal dithering approaches that were previously rejected due to timing instability should be re-tested with PIO SPI.**

### Current Transfer Bottleneck

The `oled_spi_write_blocking()` function feeds bytes to the PIO FIFO one at a time:
```c
for (size_t i = 0; i < len; i++)
    pio_sm_put_blocking(OLED_PIO, OLED_SM, (uint32_t)src[i] << 24);
```

This works but the CPU is busy-waiting on FIFO space for ~2 ms per frame. DMA would free this time for dithering computation:
```c
// DMA could feed the PIO FIFO automatically:
dma_channel_configure(chan, &cfg,
    &OLED_PIO->txf[OLED_SM],  // write to PIO TX FIFO
    frame_buffer,              // read from memory
    1024,                      // byte count
    true);                     // start
```

**Caveat:** DMA sends raw bytes to the FIFO. The PIO autopull expects data in bits 31..24 (left-justified). For DMA with 8-bit transfer size, the byte goes into bits 7..0. The PIO shift configuration would need to change, or a 32-bit DMA with pre-packed words (4× memory, zero CPU during transfer) could be used.

---

## 3. Perceived Resolution Problem Analysis

The complaint that "resolution isn't enough" on a 128×64 display is really about **perceived tonal resolution and edge definition**, not pixel count. The key factors:

### 3.1 Physical Pixel Visibility

At 0.43 mm pitch and ~30 cm viewing distance, individual pixels subtend ~5 arcminutes — well above the eye's ~1 arcminute resolution limit. **Every pixel is individually visible.** This means:
- Dithering patterns (Bayer crosshatch, FS worms) are visually apparent, not blended by the eye
- Error diffusion's organic texture feels higher-resolution than ordered dithering's grid
- The physical pixel grid itself contributes to a "low-res" perception regardless of dithering

### 3.2 Tonal Resolution Impact

With pure 1-bit spatial dithering, the system trades spatial resolution for tonal resolution:
- **Bayer 4×4** with its 4×4 repeating pattern effectively cuts perceived resolution to 32×16 "grey blocks"
- **Error diffusion** (Atkinson/FS) maintains full spatial resolution for edges while creating grey through scattered individual pixels — this is why Atkinson (mode 0) looks higher-resolution despite fewer grey levels
- **Blue noise** preserves spatial frequency but only achieves 2 grey levels per pixel per frame

### 3.3 Dark Areas Kill Perceived Resolution

DOOM has many dark corridors (E1M1, E1M3). When the shadow-lift gamma doesn't sufficiently boost dark tones, large areas of the screen are pure black. A 128×64 screen that's 40% black feels like a 77×38 screen. The shadow-lift LUT (gamma 0, pow(0.5)) helps but may still not be aggressive enough.

### 3.4 HUD Text Readability

The status bar occupies the bottom ~12 rows (19% of screen). At 128×64, text is ~5-6 pixels tall. Dithering the HUD area makes text shimmer and reduces readability, contributing to the "not enough resolution" feeling. Mode 5 (Hybrid Atkinson/HUD) addresses this by using hard thresholding below `HUD_Y_START`.

---

## 4. Recommended Rendering Pipeline

For the best possible result on this specific hardware:

```
PLAYPAL → BT.601 luminance → display_palette[256]   (once per palette change)
                                ↓
frame_buffer[128×64]  (8-bit paletted indices, Core 0)
                                ↓
combined_lut[256] = remap_lut[display_palette[i]]    (precomputed, one indirection)
                                ↓
Atkinson error diffusion with BN perturbation (mode 9, re-tuned)
   OR: 2-phase Bayer temporal (new mode 15, for evaluation)
                                ↓
Pack directly into SSD1309 page format (1024 bytes)
                                ↓
PIO SPI DMA transfer (2 ms, CPU-free)
```

### What to Implement First

**Priority 1 — Re-test mode 0 (Atkinson) and mode 9 (BN+Atkinson) with PIO SPI.**
The PIO SPI's consistent timing may already resolve shimmer issues that were attributed to the dithering algorithm but were actually SPI timing jitter.

**Priority 2 — Raise contrast to 0xDF.**
The datasheet example uses 0xDF. We're at 0xCF. This 16-step increase may noticeably improve brightness and perceived detail, especially in dark corridors.

**Priority 3 — Eliminate software Y-flip by switching to 0xC8.**
Saves 8192 subtractions per frame, potentially enabling more complex dithering within the same time budget.

**Priority 4 — Implement DMA for PIO SPI transfer.**
Frees ~2 ms of Core 1 CPU time per frame. This time can be used for more sophisticated dithering (e.g., 2-pass algorithms, temporal pre-computation).

**Priority 5 — Implement 2-phase temporal Bayer as a new mode for A/B comparison.**

---

## 5. Temporal Dithering Strategy

### 5.1 Why 2-Phase (Not 3 or 4)

The SSD1309 internally scans at ~118 Hz. Our SPI transfer takes ~2.0 ms per frame.

| Phases | Sub-frame period | Write rate | vs. SSD1309 scan | Flicker risk |
|--------|-----------------|------------|------------------|--------------|
| 1 (spatial only) | N/A | As needed | N/A | None |
| **2** | **~8.3 ms** | **~120 Hz** | **~1:1 with scan** | **Low** |
| 3 | ~5.5 ms | ~180 Hz | 1.5× scan rate | Medium — mode 3's problem |
| 4 | ~4.2 ms | ~240 Hz | 2× scan rate | High — guaranteed rolling bands |

**2-phase is correct.** Each sub-frame has roughly one full SSD1309 scan cycle to be displayed before the next sub-frame arrives. At 3+ phases, we're writing faster than the display can scan, causing the GRAM to contain a mix of two sub-frames mid-scan — this creates horizontal rolling bands.

### 5.2 Threshold Offset (Not Bitplane Contrast Switching)

**Do NOT use the upstream bitplane/contrast approach** (per-sub-frame contrast register writes). This requires:
1. Sending a contrast command mid-frame sequence — the SSD1309 processes this asynchronously to its internal scan, causing the contrast to change partway through a visible frame
2. Quantizing to 3 bits (only 7 grey levels) — less than Bayer 4×4's 17 spatial levels
3. The MUX parking trick that made this work on SSD1306 72×40 is impossible on SSD1309 128×64

**Instead: offset the spatial dither threshold between phases.** Both sub-frames use the same Bayer (or blue noise) matrix, but phase 1 adds a small constant:

```c
// Phase 0: standard threshold
if (lum > bayer4x4[sy & 3][x & 3]) pixel = ON;

// Phase 1: offset threshold → additional grey levels in mid-tones
if (lum > bayer4x4[sy & 3][x & 3] + TEMPORAL_OFFSET) pixel = ON;
```

With `TEMPORAL_OFFSET = 8` and Bayer 4×4 (17 spatial levels), only pixels near threshold boundaries toggle between phases. The image is ~95% stable with ~5% of pixels providing additional tonal steps.

### 5.3 Phase Distribution to Minimize Flicker

**Key insight:** The two sub-frames should be as visually similar as possible. Small `TEMPORAL_OFFSET` means:
- Most pixels are identical in both phases (stable)
- Only pixels within the offset window of a threshold boundary toggle
- The eye integrates the toggling pixels as an intermediate grey level

| TEMPORAL_OFFSET | Pixels that toggle | Visible flicker | Added grey levels |
|-----------------|-------------------|-----------------|-------------------|
| 0 | 0% | None | 0 |
| 4 | ~3% | Negligible | ~2-3 |
| **8** | **~6%** | **Borderline** | **~4-6** |
| 16 | ~12% | Noticeable on uniform surfaces | ~8-10 |
| 32 | ~25% | Obvious shimmer | ~14+ |
| 128 | ~50% | Severe — near-complement | Maximum but unusable |

**Start with TEMPORAL_OFFSET = 8. Tune experimentally: 4, 8, 12, 16.**

---

## 6. Spatial Dithering Comparison for This Hardware

### For a fast-moving game on a tiny OLED where "perceived resolution" is the primary concern:

| Method | Perceived Resolution | Grey Levels | Motion Stability | CPU Cost | Best For |
|--------|---------------------|-------------|------------------|----------|----------|
| **Atkinson** (mode 0) | **Highest** | Medium (6/8 error) | Moderate — swimming | Low | Perceived detail |
| BN+Atkinson (mode 9) | High | Medium | Good | Low | Best balance |
| Floyd-Steinberg (mode 6) | High | **Highest** (100% error) | Poor — wormy | Low | Gradients |
| **Blue noise 16×16** (mode 1) | High | Low (2 per pixel) | **Best** | Trivial | Stability |
| Bayer 4×4 (mode 10) | **Low** — grid visible | Good (17) | **Best** | Trivial | Motion stability |
| Bayer 8×8 (mode 11) | Low — softer grid | Better (65) | **Best** | Trivial | Gradients + motion |

**For maximizing perceived resolution: Atkinson (mode 0) or BN+Atkinson (mode 9).**

Error diffusion preserves edge detail because it distributes quantization error to neighbors rather than imposing a fixed grid pattern. The result maintains full spatial resolution for high-contrast edges (weapon silhouettes, doorframes, imps against walls) while using scattered individual pixels for intermediate tones.

**Bayer is the worst choice for perceived resolution** despite being best for motion stability. The 4×4 repeating grid turns the 128×64 display into an effective 32×16 grid of "brightness zones" — this directly reduces perceived resolution and is likely contributing to the current dissatisfaction.

### Recommendation for "best possible result"

**Mode 9 (BN+Atkinson) with re-tuned parameters:**
- Atkinson's clean edges preserve perceived spatial resolution
- Blue noise perturbation breaks the "swimming" artifacts that plague pure Atkinson in motion
- The organic BN texture feels natural at this pixel pitch
- Re-tune `BN_MODULATION` from 48 to test range 24-64 with PIO SPI

---

## 7. Scanline / Rolling Band Artifact Mitigation

### What Causes the Interaction

During a ~2.0 ms SPI write, the SSD1309's internal scan pointer advances through:
```
rows_during_write = 64 × (2.0 / 8.5) ≈ 15 rows
```

So ~15 rows of the display show a mix of "old GRAM" and "new GRAM" at any given moment during a write. For spatial-only dithering (same content repeated), this is invisible — the old and new frames are identical.

For temporal dithering, those ~15 rows show the wrong sub-frame, creating a faint horizontal band.

### Concrete Mitigation Steps

1. **Small TEMPORAL_OFFSET (≤ 16):** The two sub-frames differ minimally → the "wrong" band is nearly invisible
2. **Constant write cadence:** Use `sleep_until()` with fixed-period deadlines, not "write as fast as possible":
   ```c
   absolute_time_t next = get_absolute_time();
   while (1) {
       write_frame(sub_frame[phase]);
       phase ^= 1;
       next = delayed_by_us(next, 8333);  // 120 Hz
       sleep_until(next);
   }
   ```
3. **Always full-frame writes in same page order:** Never do partial updates. The `command_park` reset to page 0, column 0 is correct.
4. **Do NOT try to synchronize to the internal scan:** There's no VSYNC. The SSD1309 status register doesn't expose scan position. Attempting timing tricks adds complexity with no reliable benefit.

### PIO SPI Advantage

The PIO transfer has zero jitter within a frame — each byte takes exactly the same number of cycles. This means the ~15-row "transition band" is at a consistent position relative to the SSD1309 scan. If the two clocks (PIO and SSD1309 oscillator) are close in frequency, the band stays nearly fixed rather than drifting, making it less visible.

---

## 8. RP2350 / PIO-Specific Implementation Advice

### Core 1 Display Loop Structure (Current)

```c
static void core1() {
    while (true) {
        sem_acquire_blocking(&vsync);   // wait for game frame
        uint8_t *fb = frame_buffer[display_frame_index];

        // Dither + pack into frame[1024]
        // ... (mode-specific) ...

        sem_release(&vsync);            // release Core 0

        do {
            // SPI: command_park + frame data
            gpio_put(CS, 0);
            gpio_put(DC, 0);
            oled_spi_write_blocking(command_park, 6);
            gpio_put(DC, 1);
            oled_spi_write_blocking(frame, 1024);
            gpio_put(CS, 1);
            __dmb();
        } while (display_frame_index == last_fi);
    }
}
```

### Proposed Structure for Temporal Dithering

```c
static void core1() {
    absolute_time_t next_subframe = get_absolute_time();
    uint8_t sub_frame[2][1024];
    uint8_t phase = 0;

    while (true) {
        sem_acquire_blocking(&vsync);
        uint8_t *fb = frame_buffer[display_frame_index];
        uint8_t last_fi = display_frame_index;

        // Build both sub-frames once per game frame (~1.5 ms each)
        build_subframe(sub_frame[0], fb, 0);
        build_subframe(sub_frame[1], fb, TEMPORAL_OFFSET);

        sem_release(&vsync);  // Core 0 can render next frame

        // Alternate sub-frames at fixed cadence until new game frame
        do {
            gpio_put(CS, 0);
            gpio_put(DC, 0);
            oled_spi_write_blocking(command_park, 6);
            gpio_put(DC, 1);
            oled_spi_write_blocking(sub_frame[phase], 1024);
            gpio_put(CS, 1);

            phase ^= 1;

            next_subframe = delayed_by_us(next_subframe, SUBFRAME_PERIOD_US);
            sleep_until(next_subframe);
            __dmb();
        } while (*(volatile uint8_t *)&display_frame_index == last_fi);
    }
}
```

### Memory Budget

| Buffer | Size | Purpose |
|--------|------|---------|
| `sub_frame[2][1024]` | 2048 bytes | Two pre-built 1-bit sub-frames |
| `err_buf[3][128]` | 384 bytes | Atkinson error buffers (if using ED) |
| `combined_lut[256]` | 256 bytes | Pre-composed palette→gamma LUT |
| Total | ~2.7 KB | Well within RP2350's 520 KB SRAM |

### Optimization Priorities

1. **Combined LUT:** Precompute `combined_lut[i] = remap_lut[display_palette[i]]` once per palette change. Eliminates one indirection (8192 lookups) per frame per sub-frame.

2. **Hardware Y-flip (0xC8):** Switch from `0xC0` + software `y = 63 - (p*8+b)` to `0xC8` hardware COM scan remap. Saves 8192 subtractions per frame. **Test this carefully** — the page-walk order may need to change since the SSD1309 bit ordering within each byte is affected by COM scan direction.

3. **`__not_in_flash_func` / SRAM placement:** The dither inner loop should run from SRAM (already done for `oled_spi_write_blocking`). XIP flash cache misses during the tight inner loop would add jitter.

4. **DMA transfer:** Free ~2 ms of CPU time per sub-frame. Use DMA completion callback or poll in the sleep loop.

5. **Branchless threshold comparison:**
   ```c
   col >>= 1;
   col |= (lum > thr) << 7;  // ARM conditional: BIC/ORR, no branch
   ```

---

## 9. Concrete Algorithm Proposals

### Proposal A: Re-tuned BN+Atkinson (Spatial Only) — Recommended First

This is mode 9 with parameter adjustments and PIO SPI re-testing.

**Changes from current mode 9:**
- Raise contrast from 0xCF to 0xDF
- Re-tune `BN_MODULATION`: test 24, 32, 48, 64
- Re-tune `DITHER_THRESHOLD`: test 100, 110, 120
- Add combined LUT optimization
- Consider hardware Y-flip

**Expected result:** Highest perceived resolution. Atkinson preserves edge detail. BN perturbation prevents swimming. PIO SPI consistency may resolve any shimmer from the software SPI era.

**Failure mode:** If swimming is still noticeable during lateral strafing on uniform walls.

### Proposal B: 2-Phase Temporal Bayer 4×4 — For Comparison

New mode 15 with threshold offset temporal dithering.

```c
#define TEMPORAL_OFFSET      8
#define SUBFRAME_PERIOD_US   8333  // 120 Hz

// Bayer 4×4 threshold matrix (standard, scaled 0-255)
static const uint8_t bayer4x4[4][4] = {
    {  8, 136,  40, 168},
    {200,  72, 232, 104},
    { 56, 184,  24, 152},
    {248, 120, 216,  88}
};

void build_subframe(uint8_t *out, const uint8_t *fb, uint8_t t_offset) {
    memset(out, 0, 1024);
    for (int p = 0; p < 8; p++) {
        for (int x = 0; x < 128; x++) {
            uint8_t col = 0;
            for (int b = 0; b < 8; b++) {
                int sy = p * 8 + b;
                int fb_y = 63 - sy;  // software Y-flip (eliminate with 0xC8)
                uint8_t lum = combined_lut[fb[fb_y * 128 + x]];
                uint16_t thr = bayer4x4[sy & 3][x & 3] + t_offset;
                if (thr > 255) thr = 255;
                col >>= 1;
                if (lum > (uint8_t)thr) col |= 0x80;
            }
            out[p * 128 + x] = col;
        }
    }
}
```

**Expected result:** ~24-28 distinguishable grey levels (vs. 17 spatial-only Bayer). Rock-solid motion stability. But the Bayer crosshatch grid remains visible and reduces perceived resolution — this may not solve the core complaint.

**Failure mode:** Rolling horizontal bands visible on static scenes. Bayer grid making the image look "lower resolution" than Atkinson despite more grey levels.

### Proposal C: 2-Phase Temporal Blue Noise — Best of Both

Replace the Bayer matrix with the existing 16×16 blue noise matrix. Same 2-phase offset approach.

```c
void build_subframe(uint8_t *out, const uint8_t *fb, uint8_t t_offset) {
    memset(out, 0, 1024);
    for (int p = 0; p < 8; p++) {
        for (int x = 0; x < 128; x++) {
            uint8_t col = 0;
            for (int b = 0; b < 8; b++) {
                int sy = p * 8 + b;
                int fb_y = 63 - sy;
                uint8_t lum = combined_lut[fb[fb_y * 128 + x]];
                uint16_t thr = blue_noise[sy & 15][x & 15] + t_offset;
                if (thr > 255) thr = 255;
                col >>= 1;
                if (lum > (uint8_t)thr) col |= 0x80;
            }
            out[p * 128 + x] = col;
        }
    }
}
```

**Expected result:** Organic texture (no grid), stable in motion, ~3-4 extra grey levels from temporal. The blue noise is already proven (mode 1) but currently limited to 2 grey levels per pixel — temporal doubles this without adding spatial artifacts.

**Failure mode:** The 16×16 BN tile may create visible periodicity on large uniform walls (repeats every 16 pixels = 8 tiles across 128-pixel width). Rolling-bar visibility similar to Proposal B.

---

## 10. Prototype Plan

### Stage 1: Quick Wins (No Code Changes to Dithering)

- [ ] **Re-test mode 0 (Atkinson) with PIO SPI** — baseline with new SPI path
- [ ] **Re-test mode 9 (BN+Atkinson) with PIO SPI** — baseline hybrid
- [ ] **Raise contrast 0xCF → 0xDF** — single byte change in `command_initialise[]`
- [ ] **Test contrast at 0xFF** (absolute max) — check if it improves or washes out
- [ ] Photograph E1M1 dark corridor, sky gradient, weapons, HUD for each

**Measure:** Is the image noticeably better than what was tested before PIO SPI? Is the "low resolution" feeling reduced by higher contrast?

### Stage 2: Combined LUT + Hardware Y-Flip

- [ ] Implement `combined_lut[256]` precomputation — eliminates one indirection per pixel
- [ ] Test 0xC8 COM scan direction — **measure carefully**: page-walk order and bit packing must be verified
- [ ] Time the dithering inner loop with GPIO toggle on oscilloscope
- [ ] Compare mode 0 vs mode 9 frame render time

**Measure:** CPU time savings. Does hardware Y-flip change visible orientation?

### Stage 3: Temporal Dithering Evaluation

- [ ] Implement 2-phase temporal Bayer as mode 15 (Proposal B)
- [ ] Implement 2-phase temporal Blue Noise as mode 16 (Proposal C)
- [ ] Use fixed-cadence `sleep_until()` at 120 Hz
- [ ] Test `TEMPORAL_OFFSET` = 4, 8, 16 with each approach
- [ ] Photograph static scenes for grey level comparison
- [ ] Record 240 fps slow-motion video to check for rolling bars

**Decision gate:** If temporal modes 15/16 do not visibly beat mode 9 (BN+Atkinson) in person, playing E1M1, delete them. The added complexity is not justified for marginal improvement.

### Stage 4: DMA Transfer (If Temporal Is Worth Keeping)

- [ ] Implement DMA feeding PIO FIFO
- [ ] Measure CPU freed during SPI transfer (~2 ms per sub-frame)
- [ ] Use freed CPU time for dual-buffer: build sub-frame B while sub-frame A transfers

**Measure:** Frame rate improvement. Any DMA-induced timing artifacts.

---

## 11. Improving Perceived Resolution Beyond Dithering

Since the core complaint is perceived resolution, and dithering can only do so much:

### 11.1 Edge Enhancement (Mode 4 Revisited)

Mode 4 (BN+Edge) applies an unsharp mask before thresholding:
```c
mean = (c * 4 + left + right + up + down) >> 3;
boosted = c + (((c - mean) * EDGE_STRENGTH) >> 7);
```

This sharpens edges, making them more distinct against the background. The risk is halos around high-contrast edges, but at 128×64 the individual pixels are large enough that subtle halos are invisible.

**Consider adding edge enhancement to the Atkinson pipeline (mode 0 or 9)** — apply the unsharp mask to luminance before error diffusion. This could be a new mode 17: "Edge-Enhanced BN+Atkinson."

### 11.2 Adaptive Threshold by Region

The HUD region (bottom ~12 rows) contains text and numbers that benefit from hard thresholding (no dithering). Mode 5 (Hybrid HUD) does this but only works with Atkinson. A more general approach:

```c
if (sy >= HUD_Y_START) {
    // Hard threshold for text readability
    out = (lum > HUD_THRESHOLD) ? 1 : 0;
} else {
    // Full dithering for 3D viewport
    out = dither(lum, sy, sx);
}
```

This should be added to whichever final dithering mode is selected.

### 11.3 Render Resolution vs Display Resolution

The Doom engine currently renders at 128×64 (native OLED resolution). An alternative approach used by meadiode's EL display port renders at **320×256** and downscales with dithering to the physical resolution. This provides anti-aliasing at the edges — textures and geometry have sub-pixel detail that the dithering can exploit.

On RP2350 with 520 KB SRAM, rendering at 256×128 (2× in each dimension) and downscaling with area averaging before dithering would:
- Require 32 KB for the higher-res framebuffer (vs 8 KB currently)
- Provide anti-aliased edges and smoother texture sampling
- Cost ~2× rendering time on Core 0

This is a significant architectural change but would provide the single biggest improvement to perceived resolution. **Worth investigating if all dithering improvements plateau.**

### 11.4 Oscillator Frequency Tuning

Our init uses `0xD5 0xF0` (max oscillator, fastest internal refresh). The datasheet example uses `0xD5 0xA0`. A slower internal scan:
- Increases the time each row is driven → potentially brighter/more uniform
- Reduces the internal refresh rate → more time per scan cycle
- Could reduce the tearing band width during SPI writes

**Try `0xD5 0xA0` (datasheet recommended) and compare image quality.** The lower refresh rate may actually look better for static scenes because each pixel has more current drive time per cycle.

---

## 12. Honest Assessment

### Best practical method for maximizing perceived quality

1. **BN+Atkinson (mode 9)** with raised contrast (0xDF), re-tuned BN_MODULATION, and PIO SPI. This preserves the most spatial detail, produces organic dither texture, and handles motion reasonably. Re-test with PIO SPI first before changing anything else.

2. **Edge-enhanced BN+Atkinson** (new mode) if mode 9 alone still feels "not enough." The unsharp mask before dithering visually sharpens silhouettes and texture edges.

3. **Hybrid HUD variant** of whichever mode wins — hard threshold the bottom status bar for text clarity.

### Is temporal dithering worth it?

**Probably not for the primary complaint.** The "resolution isn't enough" feeling comes from:
- 128×64 physical resolution at 2.42" — each pixel is 0.43mm
- Bayer grid patterns (if used) reducing effective resolution
- Dark areas losing all detail
- HUD text being inherently hard to read at 5-pixel font height

Temporal dithering adds grey levels but doesn't increase spatial detail. It could help with gradient smoothness (sky, dimly lit walls) but won't make text more readable or edges sharper. The complexity cost (2× frame bandwidth, rolling-bar risk, cadence management) is high for marginal perceived improvement.

### When to abandon temporal dithering

After the Stage 3 prototype, if any of these are true:
- Rolling bands visible on static scenes
- No visible grey level improvement in photographs
- Shimmer/flicker noticeable during 5-second wall stare
- Mode 9 (BN+Atkinson spatial) looks equal or better in person during gameplay

### The path to "best possible result"

1. Re-test modes 0 and 9 with PIO SPI → **may already be good enough**
2. Raise contrast to 0xDF → free brightness improvement
3. Add combined LUT → free speed improvement
4. Try edge enhancement + Atkinson → maximum perceived detail
5. Add HUD hard-threshold to final mode → crisp status bar
6. Only then consider temporal dithering → if gradients are still too coarse
7. Consider 2× render resolution → nuclear option for perceived resolution, major architectural change
