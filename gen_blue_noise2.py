"""Generate a 16x16 void-and-cluster blue noise threshold texture.
Pure Python (no scipy needed). Uses toroidal Gaussian filter."""

import math

SIZE = 16

def gaussian_filter_toroidal(pattern, sigma=1.5):
    """Simple toroidal Gaussian filter for SIZE x SIZE grid."""
    radius = int(math.ceil(sigma * 3))
    kernel = []
    ksum = 0.0
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            w = math.exp(-(dx*dx + dy*dy) / (2.0 * sigma * sigma))
            kernel.append((dy, dx, w))
            ksum += w
    # Normalize
    kernel = [(dy, dx, w/ksum) for dy, dx, w in kernel]
    
    result = [[0.0]*SIZE for _ in range(SIZE)]
    for y in range(SIZE):
        for x in range(SIZE):
            val = 0.0
            for dy, dx, w in kernel:
                ny = (y + dy) % SIZE
                nx = (x + dx) % SIZE
                val += pattern[ny][nx] * w
            result[y][x] = val
    return result

def void_and_cluster():
    import random
    random.seed(42)
    
    # Initialize with sparse random pattern
    pattern = [[0]*SIZE for _ in range(SIZE)]
    initial_set = []
    for y in range(SIZE):
        for x in range(SIZE):
            if random.random() < 0.1:
                pattern[y][x] = 1
                initial_set.append((y, x))
    
    n = SIZE * SIZE
    
    # Phase 1: Remove from tightest clusters, recording order
    working = [row[:] for row in pattern]
    removal_order = []
    
    count = sum(sum(r) for r in working)
    while count > 0:
        f = gaussian_filter_toroidal(working)
        best_val = -1
        best_pos = None
        for y in range(SIZE):
            for x in range(SIZE):
                if working[y][x] and f[y][x] > best_val:
                    best_val = f[y][x]
                    best_pos = (y, x)
        working[best_pos[0]][best_pos[1]] = 0
        removal_order.append(best_pos)
        count -= 1
    
    removal_order.reverse()
    
    # Phase 2: Add into largest voids until full
    working = [row[:] for row in pattern]
    addition_order = []
    
    count = sum(sum(r) for r in working)
    while count < n:
        f = gaussian_filter_toroidal(working)
        best_val = float('inf')
        best_pos = None
        for y in range(SIZE):
            for x in range(SIZE):
                if not working[y][x] and f[y][x] < best_val:
                    best_val = f[y][x]
                    best_pos = (y, x)
        working[best_pos[0]][best_pos[1]] = 1
        addition_order.append(best_pos)
        count += 1
    
    # Build matrix
    matrix = [[0]*SIZE for _ in range(SIZE)]
    initial_count = len(removal_order)
    for rank, (y, x) in enumerate(removal_order):
        matrix[y][x] = rank
    for rank, (y, x) in enumerate(addition_order):
        matrix[y][x] = initial_count + rank
    
    # Normalize to 0..255
    for y in range(SIZE):
        for x in range(SIZE):
            matrix[y][x] = int(matrix[y][x] / (n - 1) * 255 + 0.5)
            matrix[y][x] = max(0, min(255, matrix[y][x]))
    
    return matrix

m = void_and_cluster()

print("// 16x16 void-and-cluster blue noise threshold texture")
print("// Generated with sigma=1.5, seed=42")
print("// Values span 0..255 with blue-noise spectral distribution")
print("static const uint8_t blue_noise_16x16[256] = {")
for row in range(SIZE):
    vals = m[row]
    line = "    " + ", ".join("%3d" % v for v in vals) + ","
    print(line)
print("};")

# Stats
flat = [m[y][x] for y in range(SIZE) for x in range(SIZE)]
print("// min=%d max=%d unique=%d" % (min(flat), max(flat), len(set(flat))))
