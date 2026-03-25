#!/usr/bin/env python3
"""Inspect UF2 file structure for debugging."""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else ".pio/build/doom-tbd16/firmware.uf2"
uf2 = open(path, "rb").read()
total = len(uf2) // 512
print(f"UF2: {path}")
print(f"Size: {len(uf2)} bytes, {total} blocks")

# Scan all blocks
families = set()
bad_blocks = []
fw_last_addr = 0
wad_first_addr = None

for i in range(total):
    off = i * 512
    m0, m1, flags, addr, sz, blkno, numblks, family = struct.unpack_from("<IIIIIIII", uf2, off)
    fm = struct.unpack_from("<I", uf2, off + 508)[0]
    families.add(family)

    ok = (m0 == 0x0A324655 and m1 == 0x9E5D5157 and fm == 0x0AB16F30)
    if not ok:
        bad_blocks.append((i, m0, m1, fm))

    if i == 0:
        print(f"\nBlock 0: addr=0x{addr:08x} flags=0x{flags:08x} family=0x{family:08x} numblks={numblks} sz={sz}")
    if i == total - 1:
        print(f"Block {i}: addr=0x{addr:08x} blkno={blkno} numblks={numblks} family=0x{family:08x}")

    if addr < 0x10040000:
        fw_last_addr = max(fw_last_addr, addr)
    else:
        if wad_first_addr is None:
            wad_first_addr = addr
            print(f"\nFirst WAD block at index {i}: addr=0x{addr:08x} blkno={blkno} numblks={numblks} family=0x{family:08x}")

# Check numblks consistency
first_numblks = struct.unpack_from("<I", uf2, 24)[0]
last_numblks = struct.unpack_from("<I", uf2, (total-1)*512 + 24)[0]

print(f"\nFamilies found: {', '.join(f'0x{f:08x}' for f in families)}")
print(f"RP2350 family: 0xe48bff59")
print(f"RP2040 family: 0xe48bff56")
print(f"numblks in first block: {first_numblks}")
print(f"numblks in last block: {last_numblks}")
print(f"Actual blocks in file: {total}")
print(f"numblks match actual: {first_numblks == total and last_numblks == total}")
print(f"FW last addr: 0x{fw_last_addr:08x}")
print(f"WAD first addr: 0x{wad_first_addr:08x}" if wad_first_addr else "No WAD blocks found")
print(f"Bad magic blocks: {len(bad_blocks)}")

if bad_blocks:
    for idx, m0, m1, fm in bad_blocks[:5]:
        print(f"  Block {idx}: magic0=0x{m0:08x} magic1=0x{m1:08x} final=0x{fm:08x}")

# Verify sequential block numbering
out_of_order = []
for i in range(total):
    off = i * 512
    blkno = struct.unpack_from("<I", uf2, off + 20)[0]
    if blkno != i:
        out_of_order.append((i, blkno))
print(f"Out-of-order blocks: {len(out_of_order)}")
if out_of_order:
    for idx, bn in out_of_order[:5]:
        print(f"  File position {idx} has blkno={bn}")
