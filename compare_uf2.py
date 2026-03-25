#!/usr/bin/env python3
"""Compare UF2 headers between working and broken firmware."""
import struct, sys, os

def dump_uf2(path, label):
    with open(path, 'rb') as f:
        data = f.read()
    total_blocks = len(data) // 512
    m0, m1, flags, addr, sz, blkno, numblks, family = struct.unpack_from('<IIIIIIII', data, 0)
    print(f"=== {label} ===")
    print(f"  File: {os.path.basename(path)} ({len(data)} bytes, {total_blocks} blocks)")
    print(f"  Flags:  0x{flags:08x}")
    print(f"  Addr:   0x{addr:08x}  Size: {sz}")
    print(f"  Block:  {blkno}/{numblks}")
    print(f"  Family: 0x{family:08x}")
    
    # Show first 64 bytes of payload
    payload = data[32:96]
    print(f"  Payload[0:16]: {payload[:16].hex()}")
    print(f"  Payload[16:32]: {payload[16:32].hex()}")
    
    # Check for RP2350 IMAGE_DEF block (should start with 0xffffded3)
    p32 = struct.unpack_from('<I', data, 32)[0]
    print(f"  First payload word: 0x{p32:08x}", end="")
    if p32 == 0xffffded3:
        print(" (RP2350 IMAGE_DEF marker)")
    else:
        print()
    
    # Check all blocks have same family
    families = set()
    addrs = []
    for i in range(min(total_blocks, numblks)):
        off = i * 512
        _, _, f, a, s, bn, nb, fam = struct.unpack_from('<IIIIIIII', data, off)
        families.add(fam)
        addrs.append(a)
    
    print(f"  All families: {', '.join(f'0x{f:08x}' for f in sorted(families))}")
    if addrs:
        print(f"  Address range: 0x{min(addrs):08x} - 0x{max(addrs)+256:08x}")
    
    # Check last block
    if total_blocks > 1:
        last_off = (min(total_blocks, numblks) - 1) * 512
        _, _, _, last_addr, _, last_bn, _, _ = struct.unpack_from('<IIIIIIII', data, last_off)
        print(f"  Last FW block: #{last_bn} addr=0x{last_addr:08x}")
    print()

files = [
    ("/Users/jlo/Documents/GitHub/dada-tbd-firmware/stable/pico/dada-tbd-16-v0.4.1-pico.uf2", "WORKING: stable v0.4.1"),
    ("/Users/jlo/Documents/GitHub/dada-tbd-firmware/apps/game/game-1.0.0.uf2", "WORKING: game app"),
    ("/Users/jlo/Documents/GitHub/dada-tbd-firmware/apps/ui-test/ui-test-1.0.0.uf2", "WORKING: ui-test"),
    ("/Users/jlo/Documents/GitHub/dada-tbd-app-template/.pio/build/doom-tbd16/firmware.uf2", "BROKEN: doom-tbd16"),
]

for path, label in files:
    if os.path.exists(path):
        dump_uf2(path, label)
    else:
        print(f"=== {label} === NOT FOUND: {path}\n")
