#!/usr/bin/env python3
"""Decode the Sony CMD-Z5 display bus from a DSLogic .dsl capture.

Channel mapping of capture 3 (DSLogic U3Pro32-la-260929-111226.dsl, see PINS.md):
  T0       pin 10  /WR strobe (active low, ~300-350 ns), data latched on the rising edge
  T1       pin 12  /RESET (released at ~0.595 s, first write 5 ms later)
  T2..T9   pins 17..24  data bus D7..D0
  T10      pin 9   constantly low
  T11      pin 13  A0 (0 = register index, 1 = data)
  T12      pin 14  /CS (active low)
  T13      pin 15  CS (active high)

Capture 1 (DSLogic U3Pro32-la-260929-090124.dsl) has the same data bus on T2..T9 but
no A0; use --no-a0 for it. The stream is then framed by register lengths instead:
most registers take 1 byte, 0x10 and 0x14 take 16 bytes, 0x0B (pixel data) takes 24.

Pixel data: reg 0x03 = column start (always 0), reg 0x04 = row 0..71, then 0x0B with
24 bytes = 96 pixels at 2 bpp, leftmost pixel in bits 7..6, 0 = white, 3 = black.

Usage: decode_cmdz5.py capture.dsl [outdir] [--no-a0]
Prints the register log and writes one PNG per unique full frame.
"""
import os
import sys
import zipfile

import numpy as np
from PIL import Image

CH_WR, CH_A0 = 0, 11
CH_DATA = range(9, 1, -1)                              # T9 = D0 ... T2 = D7
BURST = {0x0B: 24, 0x10: 16, 0x14: 16}
PAL = np.array([255, 170, 85, 0], np.uint8)


def load(path, channels):
    z = zipfile.ZipFile(path)
    hdr = dict(l.split(' = ', 1) for l in z.read('header').decode().splitlines() if ' = ' in l)
    n, rate, blocks = int(hdr['total samples']), float(hdr['samplerate'].split()[0]) * 1e6, int(hdr['total blocks'])
    edges = {}
    for ch in channels:
        raw = b''.join(z.read(f'L-{ch}/{i}') for i in range(blocks))
        bits = np.unpackbits(np.frombuffer(raw, np.uint8), bitorder='little')[:n]
        edges[ch] = (bits[0], np.flatnonzero(np.diff(bits)) + 1)
    return edges, rate


def level(edges, ch, t):
    init, e = edges[ch]
    return (np.searchsorted(e, t, side='right') + init) & 1


def transfers(byte, a0):
    """Yield (position, register, data bytes)."""
    if a0 is None:
        i = 0
        while i < len(byte):
            cnt = BURST.get(int(byte[i]), 1)
            yield i, int(byte[i]), [int(x) for x in byte[i + 1:i + 1 + cnt]]
            i += 1 + cnt
    else:
        idx = np.flatnonzero(a0 == 0)
        for k, i in enumerate(idx):
            end = idx[k + 1] if k + 1 < len(idx) else len(byte)
            yield i, int(byte[i]), [int(x) for x in byte[i + 1:end]]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    use_a0 = '--no-a0' not in sys.argv
    path = args[0]
    outdir = args[1] if len(args) > 1 else 'frames'
    edges, rate = load(path, [CH_WR, *CH_DATA] + ([CH_A0] if use_a0 else []))

    e0 = edges[CH_WR][1]
    falling = e0[level(edges, CH_WR, e0) == 0]        # /WR falling edges
    t = falling + 3                                   # sample mid-pulse
    byte = np.zeros(len(t), np.int64)
    for bit, ch in enumerate(CH_DATA):
        byte |= level(edges, ch, t).astype(np.int64) << bit
    a0 = level(edges, CH_A0, t) if use_a0 else None

    os.makedirs(outdir, exist_ok=True)
    fb = np.zeros((72, 96), np.uint8)
    row, last, nframe = 0, None, 0
    for i, reg, val in transfers(byte, a0):
        ts = falling[i] / rate
        if a0 is not None and reg in BURST and len(val) != BURST[reg]:
            print(f'{ts:10.6f}  note: reg {reg:02X} with {len(val)} data bytes')
        if reg == 0x04:
            row = val[0]
        elif reg == 0x0B:
            if len(val) < 24:
                break
            fb[row] = [(x >> s) & 3 for x in val[:24] for s in (6, 4, 2, 0)]
            if row == 71 and (last is None or (fb != last).any()):
                last = fb.copy()
                Image.fromarray(PAL[fb]).resize((384, 288), Image.NEAREST).save(
                    f'{outdir}/{nframe:03d}_{ts:07.3f}s.png')
                nframe += 1
        elif reg != 0x03:
            print(f'{ts:10.6f}  reg {reg:02X} = ' + ' '.join(f'{x:02X}' for x in val))
    print(f'{nframe} unique frames written to {outdir}/')


if __name__ == '__main__':
    main()
