"""Generate a 16x16 void-and-cluster blue noise threshold texture.
Uses the classic void-and-cluster algorithm for maximum spectral quality.
Output: constant C array with values 0..255 distributed to minimize
low-frequency energy (blue noise property)."""

import numpy as np
from scipy.ndimage import gaussian_filter

def void_and_cluster(size=16, sigma=1.5):
    """Generate blue noise dither matrix using void-and-cluster algorithm."""
    n = size * size
    
    # Start with roughly half the pixels set (white noise seed)
    rng = np.random.RandomState(42)  # Deterministic for reproducibility
    initial_density = 0.1
    pattern = rng.random((size, size)) < initial_density
    
    def filtered(p):
        """Gaussian-filtered version with wrap-around (toroidal)."""
        fp = np.zeros((size*3, size*3))
        for dy in range(3):
            for dx in range(3):
                fp[dy*size:(dy+1)*size, dx*size:(dx+1)*size] = p
        fp = gaussian_filter(fp.astype(float), sigma=sigma)
        return fp[size:2*size, size:2*size]
    
    # Phase 1: Remove initial minority pixels from tightest clusters
    # until empty, recording removal order
    removal_order = []
    working = pattern.copy()
    
    while working.any():
        f = filtered(working)
        # Find tightest cluster (highest filtered value among set pixels)
        f_masked = np.where(working, f, -np.inf)
        idx = np.unravel_index(np.argmax(f_masked), (size, size))
        working[idx] = False
        removal_order.append(idx)
    
    removal_order.reverse()  # First removed = highest rank
    
    # Phase 2: Add pixels into largest voids until full
    addition_order = []
    working = pattern.copy()
    
    while not working.all():
        f = filtered(working)
        # Find largest void (lowest filtered value among unset pixels)
        f_masked = np.where(~working, f, np.inf)
        idx = np.unravel_index(np.argmin(f_masked), (size, size))
        working[idx] = True
        addition_order.append(idx)
    
    # Build the final dither matrix
    # Removal order gets ranks 0 .. (initial_count - 1)
    # Addition order gets ranks initial_count .. (n - 1)
    matrix = np.zeros((size, size), dtype=int)
    
    initial_count = len(removal_order)
    for rank, (y, x) in enumerate(removal_order):
        matrix[y, x] = rank
    for rank, (y, x) in enumerate(addition_order):
        matrix[y, x] = initial_count + rank
    
    # Normalize to 0..255
    matrix = ((matrix.astype(float) / (n - 1)) * 255 + 0.5).astype(int)
    matrix = np.clip(matrix, 0, 255)
    
    return matrix

# Generate
m = void_and_cluster(16, sigma=1.5)

# Print as C array
print("// 16x16 void-and-cluster blue noise threshold texture")
print("// Generated with sigma=1.5, seed=42")
print("// Values span 0..255 with blue-noise spectral distribution")
print("static const uint8_t blue_noise_16x16[256] = {")
for row in range(16):
    vals = m[row, :]
    line = "    " + ", ".join("%3d" % v for v in vals) + ","
    print(line)
print("};")

# Verify distribution
vals = m.flatten()
print("// min=%d max=%d unique=%d" % (vals.min(), vals.max(), len(set(vals))))
print("// mean=%.1f std=%.1f" % (vals.mean(), vals.std()))
