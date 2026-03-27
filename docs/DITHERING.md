# Dithering Research Log

Observations from testing different dithering methods on the TBD-16 SSD1309 128×64 OLED.

## Quick Reference

Change mode in `lib/rp2040-doom/boards/jtbd16.h`:
```c
#define JTBD16_DITHER_MODE  DITHER_ATKINSON     // 0-14
#define JTBD16_SHADOW_GAMMA 0                    // 0-3
#define JTBD16_DITHER_THRESHOLD 110              // 0-255\n#define JTBD16_BOOT_DEBUG 0                      // 1 = show boot stages on OLED
```

Or override via build flags:
```
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=1 -DJTBD16_SHADOW_GAMMA=0" pio run -e doom-tbd16
```

## Build & Flash Commands

```bash
# Build + flash default (mode 0, gamma 0)
pio run -e doom-tbd16 -t upload

# Flash a specific mode (replace N with 0-14)
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=N" pio run -e doom-tbd16 -t upload

# Flash a specific gamma (replace N with 0-3)
PLATFORMIO_BUILD_FLAGS="-DJTBD16_SHADOW_GAMMA=N" pio run -e doom-tbd16 -t upload

# Combined: mode + gamma + threshold
PLATFORMIO_BUILD_FLAGS="-DJTBD16_DITHER_MODE=1 -DJTBD16_SHADOW_GAMMA=2 -DJTBD16_DITHER_THRESHOLD=100" \
  pio run -e doom-tbd16 -t upload

# Clean build
pio run -e doom-tbd16 -t clean && pio run -e doom-tbd16 -t upload
```

Modes: 0=Atkinson, 1=Static BN, 2=Temporal BN, 3=3-pass, 4=BN+Edge, 5=Hybrid HUD, 6=Floyd-Steinberg, 7=Sierra Lite, 8=BN+FS (hybrid), 9=BN+Atkinson (hybrid), 10=Bayer 4×4, 11=Bayer 8×8, 12=Serpentine FS, 13=JJN, 14=Stucki
Gammas: 0=aggressive(0.5), 1=moderate(0.625), 2=mild(0.8), 3=linear

## Test Log

| Date | Mode | Gamma | Threshold | Other | Observation |
|------|------|-------|-----------|-------|-------------|
| | 0 (Atkinson) | 0 (aggressive) | 110 | — | Default config. |
| | 0 (Atkinson) | 1 (moderate) | 127 | — | Previous default — too dark in corridors. |
| | 1 (static BN) | 0 | — | — | |
| | 2 (temporal BN) | 1 | — | — | Original: checkerboard/moiré on uniform walls. |
| | 3 (3-pass) | — | — | — | Reference: expected to flicker. |
| | 4 (BN + edge) | 0 | — | str=48 | |
| | 5 (hybrid) | 0 | 110 | hud_y=52, hud_thr=100 | |
| | 6 (Floyd-Steinberg) | 0 | 110 | — | |
| | 7 (Sierra Lite) | 0 | 110 | — | |
| | 8 (BN+FS hybrid) | 0 | 110 | bn_mod=48 | **Try this first** — FS grey levels + BN organic texture |
| | 9 (BN+Atkinson hybrid) | 0 | 110 | bn_mod=48 | Atkinson cleanness + BN breaks swimming |
| | 10 (Bayer 4×4) | 0 | — | — | Ordered: most stable in motion, no error cascading |
| | 11 (Bayer 8×8) | 0 | — | — | Ordered: finer grid, 64 threshold levels |
| | 12 (Serpentine FS) | 0 | 110 | — | FS with alternating scan — eliminates wormy patterns |
| | 13 (JJN) | 0 | 110 | — | 3-row 12-coeff kernel, smoothest error diffusion |
| | 14 (Stucki) | 0 | 110 | — | JJN variant, slightly sharper than JJN |

## Test Checklist

For each configuration, evaluate:
- [ ] E1M1 dark corridor (can you see imps?)
- [ ] Sky rendering (gradient smoothness)
- [ ] Uniform wall surfaces (moiré? checkerboard?)
- [ ] Weapon silhouette (visible against dark background?)
- [ ] HUD text/numbers (readable?)
- [ ] Motion quality (swimming patterns? shimmer?)
- [ ] Overall brightness (too dark? washed out?)
