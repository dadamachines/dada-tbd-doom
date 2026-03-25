#!/usr/bin/env python3
"""Generate all shadow-lift LUT variants for the dithering research framework."""
import math

configs = [
    (0, 0.50,  12, 232, "pow(0.50) — aggressive shadow lift"),
    (1, 0.625, 12, 232, "pow(0.625) — moderate shadow lift"),
    (2, 0.80,  12, 232, "pow(0.80) — mild shadow lift"),
    (3, 1.00,  12, 232, "linear — black/white point only"),
]

for idx, gamma, bp, wp, desc in configs:
    entries = []
    for i in range(256):
        if i <= bp:
            n = 0.0
        elif i >= wp:
            n = 1.0
        else:
            n = (i - bp) / float(wp - bp)
        lifted = pow(n, gamma)
        out = int(lifted * 255.0 + 0.5)
        out = max(0, min(255, out))
        entries.append(out)

    if idx == 0:
        print(f"#if JTBD16_SHADOW_GAMMA == {idx}")
    else:
        print(f"#elif JTBD16_SHADOW_GAMMA == {idx}")
    print(f"        // {desc}, bp={bp}, wp={wp}")
    print( "        static const uint8_t remap_lut[256] = {")
    for row in range(16):
        vals = entries[row * 16 : (row + 1) * 16]
        line = "            " + ", ".join("%3d" % v for v in vals) + ","
        print(line)
    print( "        };")
    # diagnostics as comment
    print(f"        // lut[{bp}]={entries[bp]} lut[30]={entries[30]} "
          f"lut[64]={entries[64]} lut[128]={entries[128]} lut[192]={entries[192]}")

# close
print("#else")
print('#error "JTBD16_SHADOW_GAMMA must be 0-3"')
print("#endif")
