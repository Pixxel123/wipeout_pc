import struct
import unittest

import numpy as np
from PIL import Image

from paths import ROOT  # noqa: F401  (scripts on the path)
import gnf


def texture(fmt, width, height, pitch, mips):
    """A GNF file as the game's are: 1D thin tiling, every level padded to whole 8x8-element tiles."""
    side, block = gnf.FORMATS[fmt][1:]
    size = 0
    for m in range(mips):
        pe = -(-(-(-max(1, pitch >> m) // side)) // 8) * 8
        he = -(-(-(-max(1, height >> m) // side)) // 8) * 8
        size += pe * he * block
    words = [0, fmt << 20 | 8, (width - 1) | (height - 1) << 14 | 0x70000000, 13 << 20 | (mips - 1) << 16 | 0x96000000,
             (pitch - 1) << 13, 0, 0, size >> 8]
    head = b'GNF ' + struct.pack('<I', 0xf8) + bytes([2, 1, 8, 0]) + struct.pack('<I', size + 0x100)
    return (head + struct.pack('<8I', *words)).ljust(0x100, b'\0') + bytes(size)


class GnfTests(unittest.TestCase):
    def test_8bit_tiles_are_morton_ordered(self):
        t = gnf.Texture(texture(1, 16, 16, 16, 1))
        image = Image.frombytes('L', (16, 16), bytes(range(256)))
        data = t.replaced(image)[0x100:]
        # Tile 0 holds rows 0-7, columns 0-7: its second element is (1, 0), its third (0, 1).
        self.assertEqual(list(data[:4]), [0, 1, 16, 17])
        self.assertEqual(data[64], 8)  # tile 1 starts at column 8
        self.assertEqual(gnf.Texture(t.replaced(image)).image().tobytes(), image.tobytes())

    def test_levels_fill_the_file_and_pad_to_tiles(self):
        t = gnf.Texture(texture(41, 88, 38, 128, 7))
        self.assertEqual([lv[1:] for lv in t.levels[:2]], [(88, 38, 32, 16), (44, 19, 16, 8)])
        self.assertEqual(t.levels[1][0] - t.levels[0][0], 32 * 16 * 16)
        with self.assertRaises(ValueError):
            gnf.Texture(texture(41, 88, 38, 128, 7)[:-1])

    def test_bc4_and_bc7_round_trip_every_level(self):
        y, x = np.mgrid[0:64, 0:64]
        gray = Image.fromarray(((x * 4) ^ (y * 4)).astype(np.uint8) & 0xf0)
        rgba = Image.fromarray(np.dstack([x * 4, y * 4, 255 - x * 4, np.where((x // 8 + y // 8) % 2, 255, 0)])
                               .astype(np.uint8), 'RGBA')
        def seen(image):  # what shows: colour times alpha (none under transparent pixels)
            a = np.asarray(image).astype(int)
            return a if a.ndim == 2 else np.dstack([a[..., :3] * a[..., 3:] // 255, a[..., 3]])

        for fmt, image, tolerance in ((38, gray, 3), (41, rgba, 6)):
            t = gnf.Texture(gnf.Texture(texture(fmt, 64, 64, 64, 7)).replaced(image))
            self.assertLess(np.abs(seen(t.image()) - seen(image)).mean(), tolerance)
            self.assertLess(np.abs(seen(t.image(2)) - seen(gnf.reduce(image, 16, 16))).mean(), tolerance)

    def test_flat_blocks_and_the_bc7_anchor_bit(self):
        # One value per block: BC4 still encodes it exactly; BC7's first index keeps its top bit 0.
        for value in (0, 128, 255):
            flat = Image.new('L', (8, 8), value)
            t = gnf.Texture(gnf.Texture(texture(38, 8, 8, 8, 1)).replaced(flat))
            self.assertEqual(set(t.image().tobytes()), {value})
        # A first pixel at either end of the block's line: at the far end its index (15) does not fit
        # the 3 bits of the first index, so the endpoints swap; either way it decodes as itself.
        for first, rest in (((255, 255, 255, 255), (0, 0, 0, 0)), ((0, 0, 0, 0), (255, 255, 255, 255))):
            image = Image.new('RGBA', (8, 8), rest)
            image.putpixel((0, 0), first)
            encoded = gnf.bc7(np.asarray(image)[:4, :4].reshape(1, 16, 4))
            self.assertEqual(encoded[0, 0] & 0x7f, 1 << 6)  # mode 6 (bit 7 is red's first bit)
            t = gnf.Texture(gnf.Texture(texture(41, 8, 8, 8, 1)).replaced(image))
            self.assertEqual(t.image().getpixel((0, 0)), first)
            self.assertEqual(t.image().getpixel((1, 0)), rest)

    def test_unsupported_formats_are_refused(self):
        with self.assertRaises(ValueError):
            gnf.Texture(texture(1, 16, 16, 16, 1)[:0x14] + struct.pack('<I', 10 << 20) + texture(1, 16, 16, 16, 1)[0x18:])
        with self.assertRaises(ValueError):
            gnf.Texture(b'DDS ' + bytes(0x100))


if __name__ == '__main__':
    unittest.main()
