import math

bp, wp = 18, 232
entries = []
for i in range(256):
    v = max(0, min(255, i))
    if v <= bp:
        n = 0.0
    elif v >= wp:
        n = 1.0
    else:
        n = (v - bp) / float(wp - bp)
    lifted = pow(n, 0.625)
    out = int(lifted * 255.0 + 0.5)
    out = max(0, min(255, out))
    entries.append(out)

print("static const uint8_t remap_lut[256] = {")
for row in range(16):
    vals = entries[row*16:(row+1)*16]
    line = "    " + ", ".join("%3d" % v for v in vals) + ","
    print(line)
print("};")
print("")
print("// bp=%d lut[0]=%d lut[%d]=%d" % (bp, entries[0], bp, entries[bp]))
print("// wp=%d lut[%d]=%d lut[255]=%d" % (wp, wp, entries[wp], entries[255]))
print("// mid: lut[64]=%d lut[128]=%d lut[192]=%d" % (entries[64], entries[128], entries[192]))
print("// dark: lut[30]=%d lut[40]=%d lut[50]=%d" % (entries[30], entries[40], entries[50]))
