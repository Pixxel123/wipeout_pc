import io
import struct
import unittest
from unittest import mock

from PIL import Image, ImageChops, ImageStat

from paths import ROOT  # noqa: F401  (scripts on the path)
import gnf
from psarc_fixtures import texture


def seen(image):
    """What shows: colour times alpha (nothing under transparent pixels)."""
    return image.convert('RGBa') if image.mode == 'RGBA' else image


def difference(a, b):
    return sum(ImageStat.Stat(ImageChops.difference(seen(a), seen(b))).mean) / len(a.getbands())


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
        gray = Image.new('L', (64, 64))
        gray.putdata([((x * 4) ^ (y * 4)) & 0xf0 for y in range(64) for x in range(64)])
        rgba = Image.new('RGBA', (64, 64))
        rgba.putdata([(x * 4, y * 4, 255 - x * 4, 255 if (x // 8 + y // 8) % 2 else 0)
                      for y in range(64) for x in range(64)])
        for fmt, image, tolerance in ((38, gray, 3), (41, rgba, 6)):
            t = gnf.Texture(gnf.Texture(texture(fmt, 64, 64, 64, 7)).replaced(image))
            self.assertLess(difference(t.image(), image), tolerance)
            self.assertLess(difference(t.image(2), gnf.reduce(image, 16, 16)), tolerance)

    def test_flat_blocks_and_the_bc7_anchor_bit(self):
        # One value per block: BC4 encodes it exactly.
        for value in (0, 1, 128, 255):
            t = gnf.Texture(gnf.Texture(texture(38, 8, 8, 8, 1)).replaced(Image.new('L', (8, 8), value)))
            self.assertEqual(set(t.image().tobytes()), {value})
        # A first pixel at either end of the block's line: at the far end its index (15) does not fit
        # the 3 bits of the first index, so the endpoints swap; either way it decodes as itself.
        for first, rest in (((255, 255, 255, 255), (0, 0, 0, 0)), ((0, 0, 0, 0), (255, 255, 255, 255))):
            image = Image.new('RGBA', (8, 8), rest)
            image.putpixel((0, 0), first)
            encoded = gnf.bc7(list(image.crop((0, 0, 4, 4)).getdata()))
            self.assertEqual(encoded[0] & 0x7f, 1 << 6)  # mode 6 (bit 7 is red's first bit)
            t = gnf.Texture(gnf.Texture(texture(41, 8, 8, 8, 1)).replaced(image))
            self.assertEqual(t.image().getpixel((0, 0)), first)
            self.assertEqual(t.image().getpixel((1, 0)), rest)

    def test_a_block_seen_before_is_not_encoded_again(self):
        image = Image.new('RGBA', (64, 64), (255, 255, 255, 0))
        for x in range(0, 64, 8):  # the same outline in every tile: one block of it to encode
            image.paste((20, 20, 20, 255), (x, 4, x + 2, 8))
        gnf.ENCODED.clear()
        with mock.patch.object(gnf, 'bc7', wraps=gnf.bc7) as encode:
            first = gnf.Texture(texture(41, 64, 64, 64, 1)).replaced(image)
            calls = encode.call_count
            again = gnf.Texture(texture(41, 64, 64, 64, 1)).replaced(image)
        self.assertLess(calls, 8)  # 256 blocks, a few distinct
        self.assertEqual(encode.call_count, calls)  # the second texture: all of them known
        self.assertEqual(first, again)

    def test_the_nearest_entry_is_found_along_the_line(self):
        # A block near its line (as a menu's outlines are): every pixel decodes to its nearest entry,
        # as a search of all 16 finds it.
        pixels = [(16 * i, 255 - 16 * i, 8 * i, 255) for i in range(16)]
        image = Image.open(io.BytesIO(gnf.dds(4, 4, 98, gnf.bc7(pixels))))
        image.load()
        got = image.convert('RGBA').tobytes()
        self.assertLess(max(abs(got[n * 4 + c] - p[c]) for n, p in enumerate(pixels) for c in range(4)), 6)

    def test_unsupported_formats_are_refused(self):
        good = texture(1, 16, 16, 16, 1)
        with self.assertRaises(ValueError):
            gnf.Texture(good[:0x14] + struct.pack('<I', 10 << 20) + good[0x18:])
        with self.assertRaises(ValueError):
            gnf.Texture(b'DDS ' + bytes(0x100))


if __name__ == '__main__':
    unittest.main()
