#!/usr/bin/env python3
"""Compare boot region of doom vs reference firmware."""
import struct

def read_fw_binary(uf2_path, max_addr=None):
    with open(uf2_path, "rb") as f:
        data = f.read()
    addrs = []
    for i in range(0, len(data), 512):
        blk = data[i:i+512]
        addr = struct.unpack_from("<I", blk, 12)[0]
        size = struct.unpack_from("<I", blk, 16)[0]
        if max_addr and addr >= max_addr:
            continue
        addrs.append((addr, blk[32:32+size]))
    addrs.sort(key=lambda x: x[0])
    base = addrs[0][0]
    end = addrs[-1][0] + len(addrs[-1][1])
    binary = bytearray(end - base)
    for addr, payload in addrs:
        binary[addr - base : addr - base + len(payload)] = payload
    return base, binary

base1, fw1 = read_fw_binary(".pio/build/doom-tbd16/firmware_only.uf2")
base2, fw2 = read_fw_binary("/Users/jlo/Documents/GitHub/dada-tbd-firmware/stable/pico/dada-tbd-16-v0.4.1-pico.uf2")

print("=== First 320 bytes comparison ===")
print("DOOM firmware:")
for off in range(0, 320, 16):
    words = struct.unpack_from("<4I", fw1, off)
    print("  {:04x}: {}".format(off, " ".join("{:08x}".format(w) for w in words)))

print("\nReference firmware:")
for off in range(0, 320, 16):
    words = struct.unpack_from("<4I", fw2, off)
    print("  {:04x}: {}".format(off, " ".join("{:08x}".format(w) for w in words)))

# Check boot2.bin
with open(".pio/build/doom-tbd16/boot2.bin", "rb") as f:
    boot2 = f.read()
print("\nboot2.bin: {} bytes".format(len(boot2)))
for off in range(0, min(64, len(boot2)), 16):
    hexs = " ".join("{:02x}".format(b) for b in boot2[off:off+16])
    print("  {:04x}: {}".format(off, hexs))

# Check if boot2 is embedded at start of firmware
if fw1[:len(boot2)] == boot2:
    print("\nboot2.bin matches start of doom firmware")
else:
    print("\nboot2.bin does NOT match start of doom firmware")
    # Find where boot2 content might be
    for off in range(0, min(4096, len(fw1)) - len(boot2)):
        if fw1[off:off+len(boot2)] == boot2:
            print("  Found boot2 at firmware offset {:04x}".format(off))
            break

# Check IMAGE_DEF details more carefully
print("\n=== IMAGE_DEF comparison ===")
def find_image_defs(binary, name):
    results = []
    for off in range(0, len(binary) - 20, 4):
        word = struct.unpack_from("<I", binary, off)[0]
        if word == 0xffffded3:
            # Read the IMAGE_DEF block
            ctx = struct.unpack_from("<8I", binary, off)
            results.append((off, ctx))
            print("{} IMAGE_DEF at {:04x}:".format(name, off))
            print("  Magic:    {:08x}".format(ctx[0]))
            print("  Item0:    {:08x}".format(ctx[1]))
            print("  Item1:    {:08x}".format(ctx[2]))
            print("  NextOfs:  {:08x} (signed: {})".format(
                ctx[3], struct.unpack("<i", struct.pack("<I", ctx[3]))[0]))
            print("  LastItem: {:08x}".format(ctx[4]))
    return results

doom_defs = find_image_defs(fw1, "DOOM")
ref_defs = find_image_defs(fw2, "REF")

# Check if the IMAGE_DEF item data differs
if doom_defs and ref_defs:
    print("\nItem0 comparison:")
    print("  DOOM: {:08x}".format(doom_defs[0][1][1]))
    print("  REF:  {:08x}".format(ref_defs[0][1][1]))
    if doom_defs[0][1][1] == ref_defs[0][1][1]:
        print("  ==> MATCH")
    else:
        print("  ==> DIFFERENT!")
