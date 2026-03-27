#!/usr/bin/env python3
"""Extract firmware-only UF2 (no WAD) and analyze RP2350 boot structure."""
import struct, sys

UF2 = ".pio/build/doom-tbd16/firmware.uf2"
WAD_ADDR = 0x10040000

with open(UF2, "rb") as f:
    data = f.read()

fw_blocks = []
wad_blocks = []
for i in range(0, len(data), 512):
    blk = data[i:i+512]
    addr = struct.unpack_from("<I", blk, 12)[0]
    if addr < WAD_ADDR:
        fw_blocks.append(bytearray(blk))
    else:
        wad_blocks.append(bytearray(blk))

print(f"Firmware blocks: {len(fw_blocks)}")
print(f"WAD blocks: {len(wad_blocks)}")

# Fix block numbering for firmware-only
total = len(fw_blocks)
for i, blk in enumerate(fw_blocks):
    struct.pack_into("<I", blk, 20, i)
    struct.pack_into("<I", blk, 24, total)

out = ".pio/build/doom-tbd16/firmware_only.uf2"
with open(out, "wb") as f:
    for blk in fw_blocks:
        f.write(blk)
print(f"Wrote {out} ({total} blocks, {total*512} bytes)")

# Analyze boot structure - look for IMAGE_DEF / block loop markers
# RP2350 IMAGE_DEF marker: 0xffffded3
# RP2350 PARTITION_TABLE marker: 0xffffbaba
# Block loop item marker start: PICOBIN_BLOCK_ITEM_1BS2_IMAGE_TYPE_IMAGE_TYPE_EXE = 0x42
print("\n=== Scanning firmware for RP2350 boot markers ===")

# Reconstruct firmware binary from UF2
fw_addrs = []
for blk in fw_blocks:
    addr = struct.unpack_from("<I", blk, 12)[0]
    size = struct.unpack_from("<I", blk, 16)[0]
    fw_addrs.append((addr, blk[32:32+size]))

fw_addrs.sort(key=lambda x: x[0])
base = fw_addrs[0][0]
end = fw_addrs[-1][0] + len(fw_addrs[-1][1])
fw_bin = bytearray(end - base)
for addr, payload in fw_addrs:
    offset = addr - base
    fw_bin[offset:offset+len(payload)] = payload

print(f"Firmware binary: {len(fw_bin)} bytes @ {base:#010x}-{end:#010x}")

# Look for RP2350 block loop markers
markers = {
    0xffffded3: "IMAGE_DEF",
    0xfffffffe: "BLOCK_LOOP_START/END",
    0xab123579: "BLOCK_ITEM_2BS_LAST",
}

# Scan first 4KB for IMAGE_DEF (RP2350 bootrom only checks first 4KB)
print("\n--- First 4KB scan ---")
for off in range(0, min(4096, len(fw_bin)), 4):
    word = struct.unpack_from("<I", fw_bin, off)[0]
    if word in markers:
        print(f"  Offset {off:#06x} ({base+off:#010x}): {word:#010x} = {markers[word]}")

# Check for embedded block / picobin block anywhere in binary
print("\n--- Full binary scan for IMAGE_DEF (0xffffded3) ---")
count = 0
for off in range(0, len(fw_bin) - 3, 4):
    word = struct.unpack_from("<I", fw_bin, off)[0]
    if word == 0xffffded3:
        print(f"  Found IMAGE_DEF at offset {off:#06x} ({base+off:#010x})")
        # Print surrounding context
        ctx_start = max(0, off - 8)
        ctx_end = min(len(fw_bin), off + 32)
        ctx_words = struct.unpack_from(f"<{(ctx_end-ctx_start)//4}I", fw_bin, ctx_start)
        print(f"  Context: {' '.join(f'{w:#010x}' for w in ctx_words)}")
        count += 1
if count == 0:
    print("  *** NO IMAGE_DEF FOUND ***")

# Also check for block loop end marker
print("\n--- Scanning for BLOCK_LOOP markers ---")
for off in range(0, len(fw_bin) - 3, 4):
    word = struct.unpack_from("<I", fw_bin, off)[0]
    if word == 0xfffffffe:
        ctx_start = max(0, off - 4)
        ctx_end = min(len(fw_bin), off + 16)
        ctx_words = struct.unpack_from(f"<{(ctx_end-ctx_start)//4}I", fw_bin, ctx_start)
        print(f"  Offset {off:#06x}: {' '.join(f'{w:#010x}' for w in ctx_words)}")

# Check vector table
print("\n--- Vector table ---")
sp, reset, nmi, hardfault = struct.unpack_from("<4I", fw_bin, 0)
print(f"  SP:        {sp:#010x}")
print(f"  Reset:     {reset:#010x}")
print(f"  NMI:       {nmi:#010x}")
print(f"  HardFault: {hardfault:#010x}")

# Compare with working firmware
import os
ref = "/Users/jlo/Documents/GitHub/dada-tbd-firmware/stable/pico/dada-tbd-16-v0.4.1-pico.uf2"
if os.path.exists(ref):
    with open(ref, "rb") as f:
        ref_data = f.read()
    
    ref_addrs = []
    for i in range(0, len(ref_data), 512):
        blk = ref_data[i:i+512]
        addr = struct.unpack_from("<I", blk, 12)[0]
        size = struct.unpack_from("<I", blk, 16)[0]
        ref_addrs.append((addr, blk[32:32+size]))
    
    ref_addrs.sort(key=lambda x: x[0])
    ref_base = ref_addrs[0][0]
    ref_end = ref_addrs[-1][0] + len(ref_addrs[-1][1])
    ref_bin = bytearray(ref_end - ref_base)
    for addr, payload in ref_addrs:
        offset = addr - ref_base
        ref_bin[offset:offset+len(payload)] = payload
    
    print(f"\n=== Reference firmware: {len(ref_bin)} bytes @ {ref_base:#010x}-{ref_end:#010x} ===")
    
    sp2, reset2, nmi2, hf2 = struct.unpack_from("<4I", ref_bin, 0)
    print(f"  SP:        {sp2:#010x}")
    print(f"  Reset:     {reset2:#010x}")
    
    print("\n--- Reference: First 4KB scan ---")
    for off in range(0, min(4096, len(ref_bin)), 4):
        word = struct.unpack_from("<I", ref_bin, off)[0]
        if word in markers:
            print(f"  Offset {off:#06x} ({ref_base+off:#010x}): {word:#010x} = {markers[word]}")
    
    print("\n--- Reference: IMAGE_DEF scan ---")
    rcount = 0
    for off in range(0, len(ref_bin) - 3, 4):
        word = struct.unpack_from("<I", ref_bin, off)[0]
        if word == 0xffffded3:
            print(f"  Found at offset {off:#06x} ({ref_base+off:#010x})")
            ctx_start = max(0, off - 8)
            ctx_end = min(len(ref_bin), off + 32)
            ctx_words = struct.unpack_from(f"<{(ctx_end-ctx_start)//4}I", ref_bin, ctx_start)
            print(f"  Context: {' '.join(f'{w:#010x}' for w in ctx_words)}")
            rcount += 1
    if rcount == 0:
        print("  *** NO IMAGE_DEF FOUND in reference ***")
