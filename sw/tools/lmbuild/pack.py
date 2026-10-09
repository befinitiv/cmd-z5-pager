#!/usr/bin/env python3
"""Pack language models into one image for the lm flash partition.

Format (little-endian): "LMPK", u32 version (1), u32 count, then per model
char lang[4], u32 offset (from the start of the pack), u32 size; the models
follow, each 4-byte aligned. Read by ti_pack_find() in lib/textinput.

  pack.py --bin lm.bin --hex lm.hex --address 0x88000 --max-size 360448 \\
      de=../../lm/de.lm en=../../lm/en.lm
"""

import argparse
import struct
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("models", nargs="+", help="lang=path")
    ap.add_argument("--bin", required=True)
    ap.add_argument("--hex", help="Intel HEX output placed at --address")
    ap.add_argument("--address", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--max-size", type=lambda s: int(s, 0))
    args = ap.parse_args()

    entries = []
    for spec in args.models:
        lang, path = spec.split("=", 1)
        with open(path, "rb") as f:
            entries.append((lang.encode(), f.read()))

    head_len = 12 + 12 * len(entries)
    offset = (head_len + 3) & ~3
    head = b"LMPK" + struct.pack("<II", 1, len(entries))
    body = b""
    for lang, data in entries:
        head += struct.pack("<4sII", lang, offset + len(body), len(data))
        body += data + bytes((-len(data)) % 4)
    image = head + bytes(offset - len(head)) + body

    if args.max_size and len(image) > args.max_size:
        sys.exit(f"pack.py: {len(image)} bytes do not fit into the lm partition "
                 f"({args.max_size} bytes)")
    with open(args.bin, "wb") as f:
        f.write(image)
    if args.hex:
        from intelhex import IntelHex

        ih = IntelHex()
        ih.frombytes(image, offset=args.address)
        ih.write_hex_file(args.hex)
    free = f", {args.max_size - len(image)} bytes free" if args.max_size else ""
    print(f"language models: {len(image)} bytes{free}")


if __name__ == "__main__":
    main()
