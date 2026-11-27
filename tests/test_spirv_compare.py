"""tools/deck/spirv-compare.py on synthetic shader stores (no game shaders).

The metas are written the way vk_pipeline_serialization.cpp writes them through common/serdes.h,
including the spare zero byte FetchShaderData::Serialize's Grow leaves at the end. The struct
sizes are made up: the tool has to measure them.
"""
import contextlib
import importlib.util
import io
import random
import struct
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT

spec = importlib.util.spec_from_file_location('spirv_compare', ROOT / 'tools/deck/spirv-compare.py')
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class Archive:
    """Serialization::Archive and Writer: a write that reaches the end grows by its own size."""

    def __init__(self):
        self.buf, self.offset = bytearray(), 0

    def grow(self, size):
        self.buf += bytes(size)

    def write(self, data):
        if self.offset + len(data) >= len(self.buf):
            self.grow(len(data))
        self.buf[self.offset:self.offset + len(data)] = data
        self.offset += len(data)


class Layout:
    def __init__(self, prefix=312, fetch=48, attribute=9, vs=8, buffer=12, image=16, fmask=8,
                 sampler=1, info=704, hash_at=640, srt=24):
        self.prefix, self.fetch, self.info, self.hash_at, self.srt = prefix, fetch, info, hash_at, srt
        self.sizes = (attribute, vs, buffer, image, fmask, sampler)


def shader(program, variant, vertex, counts=None):
    """The spec of one permutation, the same in every store: fetch attributes (None: no fetch
    data) and the counts of vertex attributes, buffers, images, fmasks and samplers."""
    rng = random.Random(program * 1000 + variant)
    if counts is None:
        counts = [rng.randint(0, 3), rng.randint(0, 2), 0, rng.randint(0, 3)]
    attributes = rng.randint(1, 4) if vertex else None
    return {'rng_seed': program * 1000 + variant, 'attributes': attributes,
            'counts': [attributes or 0] + counts}


def meta(layout, program, index, sh, garbage):
    """A meta as RegisterShaderMeta writes it; `garbage` varies the Info between stores."""
    rng = random.Random(sh['rng_seed'])
    ar = Archive()
    u32 = lambda v: ar.write(struct.pack('<I', v))
    u64 = lambda v: ar.write(struct.pack('<Q', v))
    u32(8)
    u32(15)
    u64(compare.hash_combine(program, index))
    u64(index)
    ar.write(bytes(rng.randrange(256) for _ in range(layout.prefix)))  # Bindings, RuntimeInfo
    u64(128)
    ar.write(bytes(rng.choice(b'01') for _ in range(128)))
    if sh['attributes'] is None:
        u64(0)
    else:
        u64(layout.fetch)
        ar.grow(6 + sh['attributes'] * layout.sizes[0])
        u32(16)
        ar.write(b'\xff')
        ar.write(b'\xff')
        u64(sh['attributes'])
        for _ in range(sh['attributes']):
            ar.write(bytes(rng.randrange(1, 256) for _ in range(layout.sizes[0])))
    for vector, (size, count) in enumerate(zip(layout.sizes[1:], sh['counts'])):
        u64(count)
        if vector == 4 and 'salt' in sh:  # other sampler specs, all else the same
            rng = random.Random(sh['rng_seed'] * 7 + sh['salt'])
        for _ in range(count):
            ar.write(bytes(rng.randrange(1, 256) for _ in range(size)))
    info = bytearray(garbage.randrange(256) for _ in range(layout.info))
    info[layout.hash_at:layout.hash_at + 8] = struct.pack('<Q', program)
    ar.write(bytes(info))
    words = garbage.randint(16, 40)
    u64(words)
    for _ in range(words):
        u32(garbage.randrange(1 << 32))
    walker = garbage.randint(0, 300)
    srt = struct.pack('<QQI', garbage.randrange(1 << 48), walker, words)
    ar.write(srt + bytes(layout.srt - len(srt)))
    ar.write(bytes(garbage.randrange(256) for _ in range(walker)))
    return bytes(ar.buf)


PROGRAMS = [(0x1000 + p, p % 2 == 0, 1 + p % 3) for p in range(14)]  # hash, vertex, permutations


def write_store(root, name, layout, seed, renumber=False, shaders=None):
    store = Path(root) / name
    store.mkdir()
    garbage = random.Random(seed)
    for program, vertex, count in PROGRAMS:
        indices = list(range(count))
        if renumber:  # the boot numbers permutations in its own order
            indices.reverse()
        for variant, index in enumerate(indices):
            sh = (shaders or shader)(program, variant, vertex)
            (store / f'{program:#018x}_{index}.spv').write_bytes(
                b'SPV' + struct.pack('<QQ', program, variant))
            (store / f'{compare.hash_combine(program, index):#018x}.meta').write_bytes(
                meta(layout, program, index, sh, garbage))
    return store


def run(race, boot):
    out = io.StringIO()
    argv = sys.argv
    sys.argv = ['spirv-compare.py', str(race), str(boot)]
    try:
        with contextlib.redirect_stdout(out):
            code = compare.main()
    finally:
        sys.argv = argv
    return code, out.getvalue()


class SpirvCompareTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = self.dir.name

    def tearDown(self):
        self.dir.cleanup()

    def test_the_fetch_data_grow_leaves_a_spare_byte(self):
        # As in the game: Grow(6 + attributes * 9), then u32, s8, s8, u64 count, attributes.
        for attributes in range(4):
            ar = Archive()
            ar.write(bytes(10))
            ar.grow(6 + attributes * 9)
            for size in [4, 1, 1, 8] + [9] * attributes:
                ar.write(bytes(size))
            self.assertEqual(len(ar.buf), ar.offset + 1)

    def test_same_stores_pass_and_the_layout_is_measured(self):
        race = write_store(self.root, 'race', Layout(), 1)
        boot = write_store(self.root, 'boot', Layout(), 2, renumber=True)
        code, out = run(race, boot)
        self.assertEqual(code, 0, out)
        self.assertIn('spec prefix 312 bytes', out)
        self.assertIn('fetch attribute 9, vertex attribute 8, buffer 12, image 16, fmask -, '
                      'sampler 1 bytes each, Info 704 bytes (program hash at 640), SRT info 24 '
                      'bytes, spare bytes after fetch data [1]', out)
        self.assertIn('programs in both: 14 same', out)
        self.assertIn('without a readable meta: 0', out)
        self.assertIn('metas without a SPIR-V: 0', out)
        self.assertIn('the Info, is not compared', out)

    def test_another_layout_is_measured_too(self):
        layout = Layout(prefix=400, attribute=12, vs=4, buffer=20, image=24, sampler=2, info=1000,
                        hash_at=936, srt=32)
        code, out = run(write_store(self.root, 'race', layout, 1),
                        write_store(self.root, 'boot', layout, 2))
        self.assertEqual(code, 0, out)
        self.assertIn('fetch attribute 12, vertex attribute 4, buffer 20, image 24, fmask -, '
                      'sampler 2 bytes each, Info 1000 bytes (program hash at 936), SRT info 32 '
                      'bytes, spare bytes after fetch data [2, 4]', out)

    def test_a_spec_with_the_wrong_spirv_is_a_difference(self):
        race = write_store(self.root, 'race', Layout(), 1)
        boot = write_store(self.root, 'boot', Layout(), 2)
        program = PROGRAMS[2][0]  # three permutations
        first, second = boot / f'{program:#018x}_0.spv', boot / f'{program:#018x}_1.spv'
        a, b = first.read_bytes(), second.read_bytes()
        first.write_bytes(b)
        second.write_bytes(a)
        code, out = run(race, boot)
        self.assertEqual(code, 1)
        self.assertIn('not in the race-built store: 2', out)
        self.assertIn(f'{program:#018x}: differ', out)

    def test_a_spec_that_differs_in_the_last_vector_is_a_difference(self):
        # Same SPIR-V, the samplers' spec bytes differ: the pair (spec, SPIR-V) is new.
        target = (PROGRAMS[0][0], 0)

        def shaders(salt):
            def make(program, variant, vertex):
                if (program, variant) != target:
                    return shader(program, variant, vertex)
                sh = shader(program, variant, vertex, [1, 1, 0, 2])
                return dict(sh, salt=salt) if salt else sh
            return make
        race = write_store(self.root, 'race', Layout(), 1, shaders=shaders(0))
        boot = write_store(self.root, 'boot', Layout(), 2, shaders=shaders(1))
        code, out = run(race, boot)
        self.assertEqual(code, 1, out)
        self.assertIn('not in the race-built store: 1', out)
        self.assertIn(f'{target[0]:#018x}: differ', out)

    def test_a_boot_only_program_fails(self):
        race = write_store(self.root, 'race', Layout(), 1)
        boot = write_store(self.root, 'boot', Layout(), 2)
        program, _, count = PROGRAMS[4]
        for index in range(count):
            (race / f'{program:#018x}_{index}.spv').unlink()
            (race / f'{compare.hash_combine(program, index):#018x}.meta').unlink()
        code, out = run(race, boot)
        self.assertEqual(code, 1, out)
        self.assertIn('boot only 1', out)
        self.assertIn(f'{program:#018x}: boot only', out)
        self.assertIn('not in the race-built store: 0', out)
        self.assertIn('metas without a SPIR-V: 0', out)

    def test_a_meta_without_its_spirv_fails(self):
        race = write_store(self.root, 'race', Layout(), 1)
        boot = write_store(self.root, 'boot', Layout(), 2)
        (boot / f'{PROGRAMS[2][0]:#018x}_1.spv').unlink()
        code, out = run(race, boot)
        self.assertEqual(code, 1, out)
        self.assertIn('metas without a SPIR-V: 1', out)
        self.assertIn('not in the race-built store: 0', out)

    def test_other_spirv_bytes_are_a_difference(self):
        race = write_store(self.root, 'race', Layout(), 1)
        boot = write_store(self.root, 'boot', Layout(), 2)
        spv = boot / f'{PROGRAMS[0][0]:#018x}_0.spv'
        spv.write_bytes(spv.read_bytes() + b'x')
        code, out = run(race, boot)
        self.assertEqual(code, 1)
        self.assertIn('not in the race-built store: 1', out)

    def test_cut_off_or_missing_metas_are_named_and_the_rest_compared(self):
        race = write_store(self.root, 'race', Layout(), 1)
        boot = write_store(self.root, 'boot', Layout(), 2)
        program = PROGRAMS[1][0]
        cut = boot / f'{compare.hash_combine(program, 0):#018x}.meta'
        cut.write_bytes(cut.read_bytes()[:500])
        (boot / f'{compare.hash_combine(PROGRAMS[3][0], 0):#018x}.meta').unlink()
        code, out = run(race, boot)
        self.assertEqual(code, 1)
        self.assertIn('without a readable meta: 2', out)
        self.assertIn('no meta', out)
        self.assertIn('not in the race-built store: 0', out)

    def test_metas_of_two_layouts_do_not_pass(self):
        race = write_store(self.root, 'race', Layout(), 1)
        boot = write_store(self.root, 'boot', Layout(buffer=16, info=700, hash_at=636), 2)
        code, out = run(race, boot)
        self.assertNotEqual(code, 0, out)

    def test_a_size_the_metas_cannot_tell_apart_stops_the_tool(self):
        # Every meta has one sampler, the last vector: its size and the Info's trade off, so
        # the spec's end is not known.
        one = lambda program, variant, vertex: shader(program, variant, vertex, [0, 0, 0, 1])
        race = write_store(self.root, 'race', Layout(), 1, shaders=one)
        boot = write_store(self.root, 'boot', Layout(), 2, shaders=one)
        code, out = run(race, boot)
        self.assertEqual(code, 2, out)
        self.assertIn('cannot read the metas', out)


if __name__ == '__main__':
    unittest.main()
