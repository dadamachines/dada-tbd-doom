#!/usr/bin/env python3
"""
Post-build script: append a WAD (WHD/WHX) binary into the firmware UF2.

Usage as PlatformIO extra_scripts (post:):
    Automatically runs after build. Place your .whd or .whx file as:
        data/doom1.whx   (super-tiny, shareware DOOM1)
        data/doom1.whd   (standard WHD)

Usage standalone:
    python3 append_wad_uf2.py firmware.uf2 doom1.whx [output.uf2]

The WAD binary is appended at TINY_WAD_ADDR (default 0x10040000) as
additional UF2 blocks, producing a single drag-and-drop UF2 file.
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
DATA_FAMILY_ID = 0xE48BFF58
ABSOLUTE_FAMILY_ID = 0xE48BFF57
PAYLOAD_SIZE = 256
BLOCK_SIZE = 512


def read_uf2(path):
    """Read a UF2 file, return list of 512-byte block bytearrays."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) % BLOCK_SIZE != 0:
        raise ValueError(f"UF2 file size not multiple of 512: {len(data)}")
    blocks = []
    for i in range(0, len(data), BLOCK_SIZE):
        blocks.append(bytearray(data[i : i + BLOCK_SIZE]))
    return blocks


def make_wad_blocks(wad_path, base_addr, family_id):
    """Create UF2 blocks from a raw binary WAD file."""
    with open(wad_path, "rb") as f:
        wad_data = f.read()

    # Validate WAD magic
    if len(wad_data) < 4:
        raise ValueError("WAD file too small")
    magic = wad_data[:4]
    if magic not in (b"IWAD", b"IWHD", b"IWHX"):
        raise ValueError(
            f"Not a valid WAD/WHD/WHX file (magic: {magic!r}). "
            "Run whd_gen to convert your .wad first."
        )

    blocks = []
    offset = 0
    while offset < len(wad_data):
        chunk = wad_data[offset : offset + PAYLOAD_SIZE]
        padded = chunk.ljust(476, b"\x00")  # pad data field to 476 bytes

        blk = bytearray(BLOCK_SIZE)
        struct.pack_into(
            "<8I",
            blk,
            0,
            UF2_MAGIC0,
            UF2_MAGIC1,
            UF2_FLAG_FAMILY,
            base_addr + offset,
            min(len(chunk), PAYLOAD_SIZE),
            0,  # blockNo — patched later
            0,  # numBlocks — patched later
            family_id,
        )
        blk[32 : 32 + 476] = padded
        struct.pack_into("<I", blk, 508, UF2_MAGIC_END)
        blocks.append(blk)
        offset += PAYLOAD_SIZE

    print(
        f"  WAD: {os.path.basename(wad_path)} "
        f"({len(wad_data)} bytes, {len(blocks)} UF2 blocks) "
        f"@ 0x{base_addr:08X}"
    )
    return blocks


def combine_uf2(fw_blocks, wad_blocks, output_path):
    """Merge firmware + WAD blocks, fix sequence numbers, write output."""
    all_blocks = fw_blocks + wad_blocks
    total = len(all_blocks)
    for i, blk in enumerate(all_blocks):
        struct.pack_into("<I", blk, 20, i)      # blockNo
        struct.pack_into("<I", blk, 24, total)   # numBlocks

    with open(output_path, "wb") as f:
        for blk in all_blocks:
            f.write(blk)

    print(f"  Combined UF2: {total} blocks ({total * 512} bytes) -> {output_path}")


def find_wad_file(project_dir):
    """Look for a WAD file in data/ directory."""
    data_dir = os.path.join(project_dir, "data")
    if not os.path.isdir(data_dir):
        return None
    for ext in (".whx", ".whd", ".wad"):
        for name in os.listdir(data_dir):
            if name.lower().endswith(ext):
                return os.path.join(data_dir, name)
    return None


# --- PlatformIO post-build hook ---
try:
    Import("env", "projenv")

    def append_wad_post(source, target, env):
        project_dir = env.subst("$PROJECT_DIR")
        build_dir = env.subst("$BUILD_DIR")
        uf2_path = os.path.join(build_dir, "firmware.uf2")

        if not os.path.isfile(uf2_path):
            print("  [WAD] firmware.uf2 not found, skipping")
            return

        # When USE_SD_WAD is enabled, the WAD is loaded from SD card at
        # runtime — don't embed it in the UF2 (firmware-only ~250KB).
        build_flags = env.get("BUILD_FLAGS", [])
        cppdefines = env.get("CPPDEFINES", [])
        sd_wad = False
        for item in cppdefines:
            if isinstance(item, tuple):
                if item[0] == "USE_SD_WAD":
                    sd_wad = True
                    break
            elif item == "USE_SD_WAD":
                sd_wad = True
                break
        if not sd_wad:
            # Also check raw build flags string
            flags_str = " ".join(str(f) for f in build_flags)
            sd_wad = "-DUSE_SD_WAD=1" in flags_str or "-DUSE_SD_WAD " in flags_str

        if sd_wad:
            print("  [WAD] USE_SD_WAD=1 — WAD loaded from SD card at runtime, skipping UF2 embed")
            uf2_size = os.path.getsize(uf2_path)
            print(f"  [WAD] Firmware-only UF2: {uf2_size} bytes ({uf2_size // 1024} KB)")
            return

        wad_path = find_wad_file(project_dir)
        if not wad_path:
            print(
                "  [WAD] No WAD file in data/ directory. "
                "Place a .whx or .whd file there to embed it in the UF2."
            )
            return

        print(f"  [WAD] Appending {os.path.basename(wad_path)} to UF2...")
        fw_blocks = read_uf2(uf2_path)

        # Re-stamp firmware blocks to ABSOLUTE family so all blocks
        # (firmware + WAD) share the same family ID.  The "absolute"
        # family tells the RP2350 bootloader to write each block to
        # its stated flash address, bypassing partition routing.
        for blk in fw_blocks:
            struct.pack_into("<I", blk, 28, ABSOLUTE_FAMILY_ID)

        wad_blocks = make_wad_blocks(wad_path, TINY_WAD_ADDR, ABSOLUTE_FAMILY_ID)
        combine_uf2(fw_blocks, wad_blocks, uf2_path)

    env.AddPostAction(
        os.path.join(env.subst("$BUILD_DIR"), "firmware.bin"),
        append_wad_post,
    )

except NameError:
    # Running standalone (not from PlatformIO)
    pass


# --- Standalone CLI ---
if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <firmware.uf2> <wad.whx|whd> [output.uf2]")
        sys.exit(1)

    fw_path = sys.argv[1]
    wad_path = sys.argv[2]
    out_path = sys.argv[3] if len(sys.argv) > 3 else fw_path

    fw_blocks = read_uf2(fw_path)
    wad_blocks = make_wad_blocks(wad_path, TINY_WAD_ADDR, ABSOLUTE_FAMILY_ID)
    combine_uf2(wad_blocks, fw_blocks, out_path)
