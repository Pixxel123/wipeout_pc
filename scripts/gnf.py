# SPDX-License-Identifier: GPL-2.0-or-later
"""Reads and writes WipEout's GNF textures (PS4): the pixels of every mip level, in the same bytes.

A GNF file: 'GNF ', the header size (0xf8), version 2, one texture, then the texture's T# (eight
little-endian dwords at 0x10: data format in dword 1, width and height in dword 2, tile mode and
last mip level in dword 3, pitch in dword 4) and its data at 0x100. The game's textures are tiled
1D thin: 8x8 elements (pixels, or 4x4 blocks when compressed) per tile, tiles row by row across the
pitch, the elements of a tile in Morton order (x0 y0 x1 y1 x2 y2). Each mip level is tiled on its
own (width, height and pitch halved, at least one tile) and follows the one before.

Formats: 1 = 8-bit (one channel), 38 = BC4 (one channel), 41 = BC7 (RGBA). The BC7 encoder writes
mode 6 only (one RGBA line per block, 4-bit indices): plenty for menu icons. Pillow decodes BC4 and
BC7; the encoders are plain Python (the packaged launcher has Pillow, not numpy). A block already
encoded is not encoded again (ENCODED), so a menu texture, mostly blocks of one colour and repeated
outlines, takes a few seconds.
"""
import io
from pathlib import Path
import struct

from PIL import Image

HEADER = 0x100
FORMATS = {1: ('L', 1, 1), 38: ('L', 4, 8), 41: ('RGBA', 4, 16)}  # dfmt -> (mode, block side, bytes)
DXGI = {38: 80, 41: 98}  # what Pillow's DDS reader decodes them as
BC7_WEIGHTS = (0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64)
# The index whose weight is nearest each position 0..64 along a block's line.
BC7_NEAREST = [min(range(16), key=lambda i: abs(BC7_WEIGHTS[i] - t)) for t in range(65)]
# Blocks already encoded, by format and pixels: a menu texture repeats many (its outlines, and every
# texture of one picture its whole first level), and a block costs most of the time.
ENCODED = {}


class Texture:
    def __init__(self, data):
        self.data = bytes(data)
        if self.data[:4] != b'GNF ' or len(self.data) < HEADER:
            raise ValueError('not a GNF texture')
        words = struct.unpack('<8I', self.data[0x10:0x30])
        self.format = (words[1] >> 20) & 0x3f
        self.width = (words[2] & 0x3fff) + 1
        self.height = ((words[2] >> 14) & 0x3fff) + 1
        self.tile_mode = (words[3] >> 20) & 0x1f
        self.mips = ((words[3] >> 16) & 0xf) + 1
        self.pitch = ((words[4] >> 13) & 0x3fff) + 1
        if self.format not in FORMATS:
            raise ValueError(f'GNF data format {self.format} is not supported')
        if self.tile_mode != 13:
            raise ValueError(f'GNF tile mode {self.tile_mode} is not supported (1D thin only)')
        self.mode, self.side, self.block = FORMATS[self.format]
        self.levels = []  # (offset, width, height, pitch in elements, rows in elements)
        at = HEADER
        for m in range(self.mips):
            w, h, p = max(1, self.width >> m), max(1, self.height >> m), max(1, self.pitch >> m)
            pe, he = -(-p // self.side), -(-h // self.side)
            pe, he = -(-pe // 8) * 8, -(-he // 8) * 8
            self.levels.append((at, w, h, pe, he))
            at += pe * he * self.block
        if at != len(self.data):
            raise ValueError(f'GNF size {len(self.data)}, its levels need {at}')

    def _order(self, level):
        """(element index in the tiled data of each element of the level row by row, columns, rows)."""
        _at, w, h, pe, _he = self.levels[level]
        ew, eh = -(-w // self.side), -(-h // self.side)
        morton = [(x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3
                  for y in range(8) for x in range(8)]
        tiles = pe // 8
        order = [((y >> 3) * tiles + (x >> 3)) * 64 + morton[(y & 7) * 8 + (x & 7)] for y in range(eh) for x in range(ew)]
        return order, ew, eh

    def image(self, level=0):
        """The level's pixels: an L image (8-bit, BC4) or RGBA (BC7)."""
        at, w, h, _pe, _he = self.levels[level]
        order, ew, eh = self._order(level)
        b, data = self.block, self.data
        linear = b''.join(data[at + i * b:at + i * b + b] for i in order)
        if self.format == 1:
            return Image.frombytes('L', (ew, eh), linear).crop((0, 0, w, h))
        image = Image.open(io.BytesIO(dds(ew * 4, eh * 4, DXGI[self.format], linear)))
        image.load()
        return image.convert(self.mode).crop((0, 0, w, h))

    def replaced(self, image):
        """These bytes with `image` (the size of level 0) in every level, the mips reduced from it."""
        if image.size != (self.width, self.height):
            raise ValueError(f'image {image.size}, texture {self.width}x{self.height}')
        out = bytearray(self.data)
        source = image.convert(self.mode)
        for level, (at, w, h, _pe, _he) in enumerate(self.levels):
            order, ew, eh = self._order(level)
            side = self.side
            grid = Image.new(self.mode, (ew * side, eh * side))
            pixels = reduce(source, w, h)
            grid.paste(pixels, (0, 0))
            if grid.width > w:  # edges repeated, so the last blocks encode cleanly
                grid.paste(pixels.crop((w - 1, 0, w, h)).resize((grid.width - w, h)), (w, 0))
            if grid.height > h:
                grid.paste(grid.crop((0, h - 1, grid.width, h)).resize((grid.width, grid.height - h)), (0, h))
            raw = grid.tobytes()
            b = self.block
            if self.format == 1:
                elements = [raw[i:i + 1] for i in range(len(raw))]
            else:
                channels = len(self.mode)
                row = grid.width * channels
                encode = bc4 if self.format == 38 else bc7
                known = ENCODED.setdefault(self.format, {})
                elements = []
                for by in range(eh):
                    for bx in range(ew):
                        block = []
                        for y in range(4):
                            start = (by * 4 + y) * row + bx * 4 * channels
                            line = raw[start:start + 4 * channels]
                            block += [line[i] for i in range(4)] if channels == 1 else \
                                [tuple(line[i:i + 4]) for i in range(0, 16, 4)]
                        key = tuple(block)
                        element = known.get(key)
                        if element is None:
                            element = known[key] = encode(block)
                        elements.append(element)
            for i, element in zip(order, elements):
                out[at + i * b:at + i * b + b] = element
        return bytes(out)


def reduce(image, w, h):
    if image.size == (w, h):
        return image
    if image.mode == 'RGBA':  # premultiplied, so transparent pixels do not darken the edges
        return image.convert('RGBa').resize((w, h), Image.BOX).convert('RGBA')
    return image.resize((w, h), Image.BOX)


def dds(width, height, dxgi, payload):
    header = struct.pack('<7I', 124, 0x1007 | 0x80000, height, width, len(payload), 0, 1) + bytes(44)
    pixelformat = struct.pack('<2I4s5I', 32, 4, b'DX10', 0, 0, 0, 0, 0)
    return (b'DDS ' + header + pixelformat + struct.pack('<5I', 0x1000, 0, 0, 0, 0)
            + struct.pack('<5I', dxgi, 3, 0, 1, 0) + payload)


def bc4(values):
    """A BC4 block (8 bytes) of 16 values: endpoints the block's largest and smallest value."""
    hi, lo = max(values), min(values)
    if hi == lo:  # one value: both endpoints, every index 0
        return bytes((hi, lo)) + bytes(6)
    palette = [hi, lo] + [((8 - i) * hi + (i - 1) * lo + 3) // 7 for i in range(2, 8)]
    bits = 0
    for n, v in enumerate(values):
        bits |= min(range(8), key=lambda i: abs(palette[i] - v)) << (3 * n)
    return bytes((hi, lo)) + bits.to_bytes(6, 'little')


def bc7(pixels):
    """A BC7 mode 6 block (16 bytes) of 16 RGBA pixels: the endpoints on the block's main axis."""
    if all(p == pixels[0] for p in pixels):
        ends = [list(pixels[0]), list(pixels[0])]
    else:
        mean = [sum(p[c] for p in pixels) / 16 for c in range(4)]
        centred = [[p[c] - mean[c] for c in range(4)] for p in pixels]
        cov = [[sum(q[i] * q[j] for q in centred) for j in range(4)] for i in range(4)]
        axis = [0.5, 0.5, 0.5, 0.5]
        for _ in range(8):
            axis = [sum(cov[i][j] * axis[j] for j in range(4)) for i in range(4)]
            norm = sum(a * a for a in axis) ** 0.5 or 1e-9
            axis = [a / norm for a in axis]
        t = [sum(q[c] * axis[c] for c in range(4)) for q in centred]
        ends = [[min(255.0, max(0.0, mean[c] + end * axis[c])) for c in range(4)] for end in (min(t), max(t))]
    quantized, pbits = [], []
    for end in ends:  # 7 bits a channel and one shared bit: the 8-bit value is (c << 1) | p
        best = None
        for pbit in (0, 1):
            c = [min(127, max(0, round((v - pbit) / 2))) for v in end]
            error = sum((((x << 1) | pbit) - v) ** 2 for x, v in zip(c, end))
            if best is None or error < best[2]:
                best = (c, pbit, error)
        quantized.append(best[0])
        pbits.append(best[1])
    e0 = [(c << 1) | pbits[0] for c in quantized[0]]
    e1 = [(c << 1) | pbits[1] for c in quantized[1]]
    palette = [[(a * (64 - w) + b * w + 32) >> 6 for a, b in zip(e0, e1)] for w in BC7_WEIGHTS]
    # The palette lies along the line from e0 to e1: a pixel's nearest entry is the one at its
    # position along that line, or (the entries are rounded) one of the few beside it. Three either
    # side find the nearest of all 16 for line art, and nearly always for noise.
    line = [b - a for a, b in zip(e0, e1)]
    length = sum(v * v for v in line) or 1
    index = []
    for p in pixels:
        near = BC7_NEAREST[min(64, max(0, round(64 * sum((p[c] - e0[c]) * line[c] for c in range(4)) / length)))]
        index.append(min(range(max(0, near - 3), min(16, near + 4)),
                         key=lambda i: sum((palette[i][c] - p[c]) ** 2 for c in range(4))))
    if index[0] >= 8:  # the first index has 3 bits: its top bit must be 0
        index = [15 - i for i in index]
        quantized.reverse()
        pbits.reverse()
    bits, at = 1 << 6, 7
    for c in range(4):
        for end in quantized:
            bits |= end[c] << at
            at += 7
    bits |= pbits[0] << at | pbits[1] << (at + 1)
    at += 2
    for n, i in enumerate(index):
        bits |= i << at
        at += 3 if n == 0 else 4
    return bits.to_bytes(16, 'little')


def main():
    import argparse
    ap = argparse.ArgumentParser(description='GNF texture to PNG (every mip level with --mips)')
    ap.add_argument('gnf')
    ap.add_argument('png')
    ap.add_argument('--mips', action='store_true')
    args = ap.parse_args()
    texture = Texture(Path(args.gnf).read_bytes())
    for level in range(texture.mips if args.mips else 1):
        out = Path(args.png) if not level else Path(args.png).with_suffix(f'.mip{level}.png')
        texture.image(level).save(out)
        print(f'{out}: {texture.levels[level][1]}x{texture.levels[level][2]}, format {texture.format}')


if __name__ == '__main__':
    main()
