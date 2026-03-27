import math

# Pure gamma 1.3 LUT (NO sigmoid - just shadow lift)
print('// Gamma 1.3: gentle shadow lift, no contrast manipulation')
print('static const uint8_t gamma_lut[256] = {')
for row in range(16):
    vals = []
    for col in range(16):
        i = row * 16 + col
        if i == 0:
            vals.append(0)
        else:
            val = round(255.0 * math.pow(i / 255.0, 1.0 / 1.3))
            vals.append(min(255, max(0, val)))
    print('    ' + ', '.join(f'{v:3d}' for v in vals) + ',')
print('};')

# Key values for verification
for v in [0, 16, 32, 64, 96, 128, 160, 192, 224, 255]:
    if v == 0:
        out = 0
    else:
        out = round(255.0 * math.pow(v / 255.0, 1.0 / 1.3))
    print(f'// gamma_lut[{v}] = {out}')
