#!/usr/bin/env python3
"""
pack_initrd.py - packs files into the simple archive format the kernel reads.

Usage:
    python3 pack_initrd.py output.img file1.txt file2.txt ...

Format (matches load_initrd() in kernel.c):
    for each file: 32-byte name (null padded, truncated to 31 chars) +
                   4-byte little-endian size + raw file bytes
    ends with a 32-byte all-zero name
"""
import sys
import struct

def main():
    if len(sys.argv) < 3:
        print("Usage: pack_initrd.py output.img file1 [file2 ...]")
        sys.exit(1)

    out_path = sys.argv[1]
    in_paths = sys.argv[2:]

    with open(out_path, "wb") as out:
        for path in in_paths:
            with open(path, "rb") as f:
                data = f.read()
            # use just the filename, not the full path
            name = path.split("/")[-1][:31]
            name_bytes = name.encode("ascii") + b"\x00" * (32 - len(name))
            out.write(name_bytes)
            out.write(struct.pack("<I", len(data)))
            out.write(data)
        # end marker: 32 zero bytes for the "name" field
        out.write(b"\x00" * 32)

    print(f"Wrote {out_path} with {len(in_paths)} file(s).")

if __name__ == "__main__":
    main()
