import math, random

# --- Generate optimal contrast LUT ---
lut = []
for i in range(256):
    x = i / 255.0
    g = math.pow(x, 1.0 / 1.4) if x > 0 else 0
    s = 1.0 / (1.0 + math.exp(-10.0 * (g - 0.5)))
    s0 = 1.0 / (1.0 + math.exp(5.0))
    s1 = 1.0 / (1.0 + math.exp(-5.0))
    s = (s - s0) / (s1 - s0)
    val = int(s * 255.0 + 0.5)
    val = max(0, min(255, val))
    lut.append(val)

print('static const uint8_t contrast_lut[256] = {')
for row in range(16):
    vals = lut[row*16:(row+1)*16]
    print('    ' + ', '.join(f'{v:3d}' for v in vals) + ',')
print('};')
print()

# --- Generate 16x16 blue noise via void-and-cluster ---
random.seed(42)
N = 16

grid = [[0]*N for _ in range(N)]
values = [[0]*N for _ in range(N)]

for i in range(N):
    grid[i][(i*5)%N] = 1

def gaussian_energy(gx, gy, px, py, sigma=1.5):
    dx = min(abs(gx-px), N-abs(gx-px))
    dy = min(abs(gy-py), N-abs(gy-py))
    return math.exp(-(dx*dx+dy*dy)/(2*sigma*sigma))

def calc_energy(g, x, y):
    e = 0
    for py in range(N):
        for px in range(N):
            if g[py][px] and (px != x or py != y):
                e += gaussian_energy(x, y, px, py)
    return e

total = sum(sum(row) for row in grid)

temp = [row[:] for row in grid]
for r in range(total, 0, -1):
    best_e = -1
    best = (0,0)
    for y in range(N):
        for x in range(N):
            if temp[y][x]:
                e = calc_energy(temp, x, y)
                if e > best_e:
                    best_e = e
                    best = (x,y)
    values[best[1]][best[0]] = r - 1
    temp[best[1]][best[0]] = 0

temp = [row[:] for row in grid]
for r in range(total, N*N):
    best_e = float('inf')
    best = (0,0)
    for y in range(N):
        for x in range(N):
            if not temp[y][x]:
                e = calc_energy(temp, x, y)
                if e < best_e:
                    best_e = e
                    best = (x,y)
    values[best[1]][best[0]] = r
    temp[best[1]][best[0]] = 1

flat = [values[y][x] for y in range(N) for x in range(N)]
mn, mx = min(flat), max(flat)
normalized = [int((v - mn) / (mx - mn) * 255 + 0.5) for v in flat]

print('static const uint8_t blue_noise[256] = {')
for row in range(16):
    vals = normalized[row*16:(row+1)*16]
    print('    ' + ', '.join(f'{v:3d}' for v in vals) + ',')
print('};')
