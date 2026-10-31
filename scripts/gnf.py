# SPDX-License-Identifier: GPL-2.0-or-later
"""Reads and writes WipEout's GNF textures (PS4): the pixels of every mip level, in the same bytes.

A GNF file: 'GNF ', the header size (0xf8), version 2, one texture, then the texture's T# (eight
little-endian dwords at 0x10: data format in dword 1, width and height in dword 2, tile mode and
last mip level in dword 3, pitch in dword 4) and its data at 0x100. The game's textures are tiled
1D thin: 8x8 elements (pixels, or 4x4 blocks when compressed) per tile, tiles row by row across the
pitch, the elements of a tile in Morton order (x0 y0 x1 y1 x2 y2). Each mip level is tiled on its
own (width, height and pitch halved, at least one tile) and follows the one before.

Formats: 1 = 8-bit (one channel), 38 = BC4 (one channel), 41 = BC7 (RGBA). The BC7 encoder writes
mode 6 only (one RGBA line per block, 4-bit indices): plenty for menu icons. Needs Pillow and numpy.
"""
import io
from pathlib import Path
import struct

import numpy as np
from PIL import Image

HEADER = 0x100
FORMATS = {1: ('L', 1, 1), 38: ('L', 4, 8), 41: ('RGBA', 4, 16)}  # dfmt -> (mode, block side, bytes)
DXGI = {38: 80, 41: 98}  # what Pillow's DDS reader decodes them as
BC7_WEIGHTS = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int32)


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
        """Element index in the tiled data of each element of the level, row by row."""
        _at, w, h, pe, _he = self.levels[level]
        ew, eh = -(-w // self.side), -(-h // self.side)
        y, x = np.mgrid[0:eh, 0:ew]
        morton = (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3
        return ((y // 8) * (pe // 8) + x // 8) * 64 + morton, ew, eh

    def image(self, level=0):
        """The level's pixels: an L image (8-bit, BC4) or RGBA (BC7)."""
        at, w, h, pe, he = self.levels[level]
        order, ew, eh = self._order(level)
        tiled = np.frombuffer(self.data, np.uint8, pe * he * self.block, at).reshape(-1, self.block)
        linear = tiled[order.ravel()].tobytes()
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
        for level, (at, w, h, pe, he) in enumerate(self.levels):
            pixels = reduce(image.convert(self.mode), w, h)
            order, ew, eh = self._order(level)
            grid = np.zeros((eh * self.side, ew * self.side, len(self.mode)), np.uint8)
            grid[:h, :w] = np.asarray(pixels).reshape(h, w, len(self.mode))
            grid[:h, w:] = grid[:h, w - 1:w]  # edges repeated, so the last blocks encode cleanly
            grid[h:] = grid[h - 1:h]
            if self.format == 1:
                elements = grid.reshape(-1, 1)
            else:
                blocks = grid.reshape(eh, 4, ew, 4, -1).swapaxes(1, 2).reshape(eh * ew, 16, -1)
                elements = bc4(blocks[:, :, 0]) if self.format == 38 else bc7(blocks)
            tiled = np.frombuffer(self.data, np.uint8, pe * he * self.block, at).reshape(-1, self.block).copy()
            tiled[order.ravel()] = elements
            out[at:at + tiled.size] = tiled.tobytes()
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


def bits(fields, count):
    """Blocks of `count` bits, least significant first: fields = [(values per block, width)...]."""
    out = np.zeros((len(fields[0][0]), count), np.uint8)
    at = 0
    for values, width in fields:
        values = np.asarray(values, np.int64).reshape(len(out), -1)
        for column in range(values.shape[1]):
            for bit in range(width):
                out[:, at] = (values[:, column] >> bit) & 1
                at += 1
    assert at == count
    return np.packbits(out, axis=1, bitorder='little')


def bc4(values):
    """BC4 blocks (8 bytes) of (n, 16) values: endpoints the block's largest and smallest value."""
    values = values.astype(np.int32)
    hi, lo = values.max(1), values.min(1)
    flat = hi == lo
    lo = np.where(flat, np.maximum(hi - 1, 0), lo)
    hi = np.where(flat & (hi == 0), 1, hi)
    i = np.arange(8)
    palette = np.where(i == 0, hi[:, None], np.where(i == 1, lo[:, None],
                       ((8 - i) * hi[:, None] + (i - 1) * lo[:, None] + 3) // 7))
    index = np.abs(values[:, :, None] - palette[:, None, :]).argmin(2)
    return bits([(hi, 8), (lo, 8), (index, 3)], 64)


def bc7(pixels):
    """BC7 mode 6 blocks (16 bytes) of (n, 16, 4) RGBA pixels: the endpoints on the block's main axis."""
    p = pixels.astype(np.float64)
    mean = p.mean(1, keepdims=True)
    centred = p - mean
    axis = np.ones((len(p), 4)) / 2
    cov = np.einsum('nki,nkj->nij', centred, centred)
    for _ in range(8):
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-9)
    t = np.einsum('nki,ni->nk', centred, axis)
    ends = [np.clip(mean[:, 0] + t.min(1, keepdims=True) * axis, 0, 255),
            np.clip(mean[:, 0] + t.max(1, keepdims=True) * axis, 0, 255)]
    quantized, pbits = [], []
    for end in ends:  # 7 bits a channel and one shared bit: the 8-bit value is (c << 1) | p
        best = None
        for pbit in (0, 1):
            c = np.clip(np.rint((end - pbit) / 2), 0, 127).astype(np.int32)
            error = (((c << 1) | pbit) - end) ** 2
            best = (c, np.full(len(p), pbit), error.sum(1)) if best is None else best
            take = error.sum(1) < best[2]
            best = (np.where(take[:, None], c, best[0]), np.where(take, pbit, best[1]), np.minimum(error.sum(1), best[2]))
        quantized.append(best[0])
        pbits.append(best[1])
    e0 = (quantized[0] << 1) | pbits[0][:, None]
    e1 = (quantized[1] << 1) | pbits[1][:, None]
    palette = (e0[:, None, :] * (64 - BC7_WEIGHTS[None, :, None]) + e1[:, None, :] * BC7_WEIGHTS[None, :, None] + 32) >> 6
    index = ((pixels[:, :, None, :].astype(np.int32) - palette[:, None, :, :]) ** 2).sum(3).argmin(2)
    swap = index[:, 0] >= 8  # the first index has 3 bits: its top bit must be 0
    index = np.where(swap[:, None], 15 - index, index)
    q0 = np.where(swap[:, None], quantized[1], quantized[0])
    q1 = np.where(swap[:, None], quantized[0], quantized[1])
    p0, p1 = np.where(swap, pbits[1], pbits[0]), np.where(swap, pbits[0], pbits[1])
    fields = [(np.full(len(p), 1 << 6), 7)]
    for channel in range(4):
        fields += [(q0[:, channel], 7), (q1[:, channel], 7)]
    fields += [(p0, 1), (p1, 1), (index[:, 0], 3), (index[:, 1:], 4)]
    return bits(fields, 128)


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
