#!/usr/bin/env python3
"""
Split a combined firmware+WAD UF2 into two separate UF2 files:
  1. firmware-only.uf2 — the doom engine (flashes at 0x10000000)
  2. doom1-wad.uf2     — the WAD data   (flashes at TINY_WAD_ADDR)

Also supports creating a WAD-only UF2 directly from a .whx/.whd file.

Usage:
    python3 split_uf2.py                         # split combined UF2
    python3 split_uf2.py combined.uf2            # split specific UF2
    python3 split_uf2.py --wad-only data/doom1.whx  # create WAD UF2 from raw file
"""
import struct
import os
import sys

TINY_WAD_ADDR = 0x10040000
UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
RP2350_FAMILY_ID = 0xE48BFF59
PAYLOAD_SIZE = 256
BLOCK_SIZE = 512


def read_uf2_blocks(path):
    """Read a UF2 file, return list of 512-byte block bytearrays."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) % BLOCK_SIZE != 0:
        raise ValueError(f"UF2 file size not multiple of 512: {len(data)}")
    return [bytearray(data[i : i + BLOCK_SIZE]) for i in range(0, len(data), BLOCK_SIZE)]


def write_uf2(blocks, output_path):
    """Fix sequence numbers and write UF2 blocks to file."""
    total = len(blocks)
    for i, blk in enumerate(blocks):
        struct.pack_into("<I", blk, 20, i)      # blockNo
        struct.pack_into("<I", blk, 24, total)   # numBlocks
    with open(output_path, "wb") as f:
        for blk in blocks:
            f.write(blk)
    print(f"  {output_path}: {total} blocks ({total * BLOCK_SIZE} bytes)")


def block_addr(blk):
    """Get target address from a UF2 block."""
    return struct.unpack_from("<I", blk, 12)[0]


def make_wad_uf2(wad_path, output_path, base_addr=TINY_WAD_ADDR, family_id=RP2350_FAMILY_ID):
    """Create a UF2 file from a raw WAD binary."""
    with open(wad_path, "rb") as f:
        wad_data = f.read()

    if len(wad_data) < 4:
        raise ValueError("WAD file too small")
    magic = wad_data[:4]
    if magic not in (b"IWAD", b"IWHD", b"IWHX"):
        raise ValueError(f"Not a valid WAD/WHD/WHX file (magic: {magic!r})")

    blocks = []
    offset = 0
    while offset < len(wad_data):
        chunk = wad_data[offset : offset + PAYLOAD_SIZE]
        padded = chunk.ljust(476, b"\x00")

        blk = bytearray(BLOCK_SIZE)
        struct.pack_into(
            "<8I", blk, 0,
            UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY,
            base_addr + offset,
            min(len(chunk), PAYLOAD_SIZE),
            0, 0,  # blockNo, numBlocks — fixed by write_uf2
            family_id,
        )
        blk[32 : 32 + 476] = padded
        struct.pack_into("<I", blk, 508, UF2_MAGIC_END)
        blocks.append(blk)
        offset += PAYLOAD_SIZE

    print(f"  WAD: {os.path.basename(wad_path)} ({len(wad_data)} bytes)")
    write_uf2(blocks, output_path)


def split_combined(uf2_path, out_dir):
    """Split a combined firmware+WAD UF2 into two separate files."""
    blocks = read_uf2_blocks(uf2_path)
    print(f"Input: {uf2_path} ({len(blocks)} blocks)")

    fw_blocks = [b for b in blocks if block_addr(b) < TINY_WAD_ADDR]
    wad_blocks = [b for b in blocks if block_addr(b) >= TINY_WAD_ADDR]

    if not fw_blocks:
        raise ValueError("No firmware blocks found (addr < 0x{:08X})".format(TINY_WAD_ADDR))
    if not wad_blocks:
        raise ValueError("No WAD blocks found (addr >= 0x{:08X})".format(TINY_WAD_ADDR))

    fw_start = block_addr(fw_blocks[0])
    fw_end = block_addr(fw_blocks[-1]) + PAYLOAD_SIZE
    wad_start = block_addr(wad_blocks[0])
    wad_end = block_addr(wad_blocks[-1]) + PAYLOAD_SIZE

    print(f"  Firmware: {len(fw_blocks)} blocks, 0x{fw_start:08X}–0x{fw_end:08X}")
    print(f"  WAD:      {len(wad_blocks)} blocks, 0x{wad_start:08X}–0x{wad_end:08X}")

    fw_out = os.path.join(out_dir, "firmware-only.uf2")
    wad_out = os.path.join(out_dir, "doom1-wad.uf2")

    write_uf2(fw_blocks, fw_out)
    write_uf2(wad_blocks, wad_out)
    print("\nFlash order:")
    print(f"  1. {fw_out}   (firmware at 0x{fw_start:08X})")
    print(f"  2. {wad_out}  (WAD data at 0x{wad_start:08X})")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--wad-only":
        wad_path = sys.argv[2] if len(sys.argv) > 2 else "data/doom1.whx"
        out = sys.argv[3] if len(sys.argv) > 3 else "doom1-wad.uf2"
        make_wad_uf2(wad_path, out)
    else:
        uf2_path = sys.argv[1] if len(sys.argv) > 1 else ".pio/build/doom-tbd16/firmware.uf2"
        out_dir = os.path.dirname(uf2_path) or "."
        split_combined(uf2_path, out_dir)
