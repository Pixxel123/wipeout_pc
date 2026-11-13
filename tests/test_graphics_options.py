import json
import re
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401 (puts scripts/ on sys.path)
import graphics_options as g


class GraphicsOptionsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.ini = Path(self.temp.name) / 'gow3.ini'
        self.state = Path(self.temp.name) / 'state.json'

    def tearDown(self):
        self.temp.cleanup()

    def test_defaults_and_invalid_values(self):
        self.ini.write_text(
            'render_resolution=8K\nengine_fps=90\nstale_readback=yes\n',
            encoding='utf-8',
        )
        options, reverted = g.prepare(self.ini, self.state)
        self.assertFalse(reverted)
        self.assertEqual(
            options,
            {
                'render_resolution': 'native',
                'engine_fps': '120',
                'deferred_readback': '1',
                'stale_readback': '1',
                'async_shaders': '1',
            },
        )

    def test_corrupt_state_file_preserves_current_settings(self):
        self.ini.write_text(
            'render_resolution=1440p\nengine_fps=240\n',
            encoding='utf-8',
        )
        self.state.write_text('{invalid json', encoding='utf-8')

        options, reverted = g.prepare(self.ini, self.state)

        self.assertFalse(reverted)
        self.assertEqual(options['render_resolution'], '1440p')
        self.assertEqual(options['engine_fps'], '240')

    def test_corrupt_state_file_preserves_current_settings(self):
        self.ini.write_text('render_resolution=1440p\nengine_fps=240\n', encoding='utf-8')
        self.state.write_text('{invalid json', encoding='utf-8')

        options, reverted = g.prepare(self.ini, self.state)

        self.assertFalse(reverted)
        self.assertEqual(options['render_resolution'], '1440p')
        self.assertEqual(options['engine_fps'], '240')

    def test_write_keys_keeps_other_lines(self):
        self.ini.write_text(
            '# comment\nshow_fps=1\nengine_fps=60\n',
            encoding='utf-8',
        )
        g.write_keys(self.ini, {'engine_fps': '240', 'vsync': '0'})
        self.assertEqual(
            self.ini.read_text(encoding='utf-8'),
            '# comment\nshow_fps=1\nengine_fps=240\nvsync=0\n',
        )

    def test_unconfirmed_restart_is_tried_once_then_reverted(self):
        self.ini.write_text(
            'render_resolution=1440p\nengine_fps=120\n',
            encoding='utf-8',
        )
        good, _ = g.prepare(self.ini, self.state)
        g.write_keys(
            self.ini,
            {
                'render_resolution': '4K',
                'engine_fps': '240',
                'restart_unconfirmed': '1',
            },
        )

        tried, reverted = g.prepare(self.ini, self.state)
        self.assertFalse(reverted)
        self.assertEqual(
            (tried['render_resolution'], tried['engine_fps']),
            ('4K', '240'),
        )

        back, reverted = g.prepare(self.ini, self.state)
        self.assertTrue(reverted)
        self.assertEqual(back, good)

        ini = g.read_ini(self.ini)
        self.assertEqual(
            (ini['render_resolution'], ini['restart_unconfirmed']),
            ('1440p', '0'),
        )

    def test_confirmed_restart_becomes_the_last_good(self):
        g.prepare(self.ini, self.state)
        g.write_keys(
            self.ini,
            {'engine_fps': '240', 'restart_unconfirmed': '1'},
        )
        g.prepare(self.ini, self.state)
        g.write_keys(self.ini, {'restart_unconfirmed': '0'})
        g.prepare(self.ini, self.state)

        self.assertEqual(
            json.loads(self.state.read_text())['last_good']['engine_fps'],
            '240',
        )

    def test_launch_ini_is_what_prepare_will_use(self):
        config, state = self.ini, self.state
        config.write_text('render_resolution=4K\nrestart_unconfirmed=1\n', encoding='utf-8')
        self.assertEqual(g.launch_ini(config, state)['render_resolution'], '4K')  # no state
        for broken in ('{broken', '[1]'):
            state.write_text(broken, encoding='utf-8')
            self.assertEqual(g.launch_ini(config, state)['render_resolution'], '4K', broken)
        state.write_text(json.dumps({'attempt': False, 'last_good': {'render_resolution': '2256x1504'}}),
                         encoding='utf-8')
        self.assertEqual(g.launch_ini(config, state)['render_resolution'], '4K')  # first try
        state.write_text(json.dumps({'attempt': True, 'last_good': {'render_resolution': '2256x1504'}}),
                         encoding='utf-8')
        before = (config.read_bytes(), state.read_bytes())
        self.assertEqual(g.launch_ini(config, state)['render_resolution'], '2256x1504')
        self.assertEqual((config.read_bytes(), state.read_bytes()), before)
        config.write_text('render_resolution=4K\n', encoding='utf-8')  # confirmed: the ini wins
        self.assertEqual(g.launch_ini(config, state)['render_resolution'], '4K')

    def test_patch_names(self):
        launcher = [
            'Bug Fix - Texture Corruption Fix',
            'Frame Rate Patch - 120 FPS',
            'Skip Any Video With X Button',
        ]
        options = g.startup_options(
            {'render_resolution': '1440p', 'engine_fps': '240'}
        )
        self.assertEqual(
            g.patch_names(launcher, options),
            [
                'Skip Any Video With X Button',
                'Resolution Patch - 1440p',
                'Frame Rate Patch - 240 FPS',
            ],
        )

        native = g.startup_options({'engine_fps': '60'})
        self.assertEqual(
            g.patch_names(
                ['Resolution Patch - 4K', 'Skip Any Video With X Button'],
                native,
            ),
            [
                'Bug Fix - Texture Corruption Fix',
                'Skip Any Video With X Button',
            ],
        )

    def test_generated_sizes_are_labelled_beta(self):
        self.assertEqual(g.resolution_label('2256x1504'), '2256 × 1504 · 3:2 (beta)')
        self.assertEqual(g.resolution_label('1366x768'), '1366 × 768 · 16:9 (beta)')
        self.assertEqual(g.resolution_label('1440p'), '1440p · 2560 × 1440 · 16:9')

    def test_only_a_generated_size_reads_beta(self):
        self.assertEqual(g.resolution_label('2880x1800', generated=False), '2880 × 1800 · 16:10')
        self.assertEqual(g.resolution_label('3440x1440', generated=False), '3440 × 1440 · 21:9 (beta)')  # built in: unchanged

    def test_resolution_labels_show_the_size_and_aspect_ratio(self):
        self.assertEqual(g.resolution_label('native'), 'Native · 1920 × 1080 · 16:9')
        self.assertEqual(g.resolution_label('480p'), '480p · 854 × 480 · 16:9')
        self.assertEqual(g.resolution_label('900p'), '900p · 1600 × 900 · 16:9')
        self.assertEqual(g.resolution_label('3440x1440'), '3440 × 1440 · 21:9 (beta)')
        self.assertEqual(g.resolution_label('5120x1440'), '5120 × 1440 · 32:9 (beta)')
        self.assertEqual(g.resolution_label('2560x1600'), '2560 × 1600 · 16:10 (beta)')
        self.assertEqual(g.resolution_label('1280x800'), '1280 × 800 · 16:10')  # the Steam Deck's screen, tested
        self.assertEqual(g.resolution_label('1280x960'), '1280 × 960 · 4:3')
        self.assertEqual(g.resolution_label('1920x1440'), '1920 × 1440 · 4:3 (beta)')
        self.assertEqual(g.resolution_label('3840x1080', generated=False), '3840 × 1080 · 32:9')  # a mod's size, not beta
        self.assertEqual(g.resolution_label('1600x1200', generated=False), '1600 × 1200 · 4:3')
        self.assertEqual(g.resolution_label('custom'), 'custom')
        # Every ultrawide and 16:10 size is beta; the 16:9 ones are not.
        self.assertEqual([r for r in g.RESOLUTIONS if g.resolution_label(r).endswith('(beta)')],
                         list(g.BETA_RESOLUTIONS))

    def test_every_built_in_resolution_has_a_patch(self):
        import patches
        folder = Path(__file__).resolve().parent.parent / 'patches'
        xmls = [folder / 'God_of_War_III_Remastered.xml', folder / 'WipEout_Omega_Collection.xml']
        self.assertEqual(sorted(r for r in g.RESOLUTIONS if r != 'native' and r not in patches.resolution_names(xmls)), [])

    def test_900p_is_the_built_in_size_between_720p_and_1440p(self):
        # #238: 900p (1600x900) sits between 720p and 1440p, with its own resolution patch.
        self.assertEqual(g.RESOLUTIONS.index('900p'), 3)
        self.assertEqual(g.SIZES['900p'], (1600, 900))
        import xml.etree.ElementTree as ET
        xml = Path(__file__).resolve().parent.parent / 'patches' / 'God_of_War_III_Remastered.xml'
        names = {m.get('Name') for m in ET.parse(xml).getroot().iter('Metadata')}
        self.assertIn('Resolution Patch - 900p', names)

    def test_resolutions_of_enabled_mods(self):
        # A mod's "Resolution Patch - <size>" is a render resolution only while its mod is on.
        self.assertEqual(g.resolutions(), g.RESOLUTIONS)
        self.assertEqual(g.resolutions(['3840x1080', '4K']), g.RESOLUTIONS + ('3840x1080',))
        ini = {'render_resolution': '3840x1080', 'engine_fps': '60'}
        self.assertEqual(g.startup_options(ini)['render_resolution'], 'native')
        options = g.startup_options(ini, ['3840x1080'])
        self.assertEqual(options['render_resolution'], '3840x1080')
        self.assertEqual(g.patch_names(['Bug Fix - Texture Corruption Fix'], options), ['Resolution Patch - 3840x1080'])

    def test_a_confirmed_mod_resolution_is_restored_while_its_mod_is_on(self):
        with tempfile.TemporaryDirectory() as folder:
            ini, state = Path(folder) / 'gow3.ini', Path(folder) / 'state.json'
            g.write_keys(ini, {'render_resolution': '3840x1080', 'engine_fps': '60'})
            tried, _ = g.prepare(ini, state, ['3840x1080'])
            self.assertEqual(tried['render_resolution'], '3840x1080')
            g.write_keys(ini, {'render_resolution': '4K', 'restart_unconfirmed': '1'})
            self.assertEqual(g.prepare(ini, state, ['3840x1080'])[0]['render_resolution'], '4K')  # attempt
            options, reverted = g.prepare(ini, state, ['3840x1080'])  # the restart never confirmed
            self.assertTrue(reverted)
            self.assertEqual(options['render_resolution'], '3840x1080')

    def test_engine_rate_100_for_100hz_displays(self):
        self.assertIn(100, g.ENGINE_FPS)
        options = g.startup_options({'render_resolution': 'native', 'engine_fps': '100'})
        self.assertEqual(options['engine_fps'], '100')
        self.assertEqual(g.patch_names(['Bug Fix - Texture Corruption Fix'], options),
                         ['Bug Fix - Texture Corruption Fix', 'Frame Rate Patch - 100 FPS'])
        self.assertEqual(g.startup_options({'engine_fps': '90'})['engine_fps'], '120')  # not a choice: the default

    def test_applied_engine_fps(self):
        self.assertEqual(
            g.applied_engine_fps(
                ['Skip Any Video With X Button', 'Frame Rate Patch - 240 FPS']
            ),
            240,
        )
        self.assertEqual(
            g.applied_engine_fps(['Frame Rate Patch - 120 FPS']),
            120,
        )
        self.assertEqual(
            g.applied_engine_fps(['Frame Rate Patch - 100 FPS']),
            100,
        )
        self.assertEqual(
            g.applied_engine_fps(['Bug Fix - Texture Corruption Fix']),
            60,
        )
        self.assertEqual(g.applied_engine_fps([]), 60)

    def test_environment(self):
        env = g.environment(
            g.startup_options({'stale_readback': '0'})
        )
        self.assertEqual(
            env,
            {
                'GOW3_DEFERRED_READBACK': '1',
                'GOW3_STALE_READBACK': '0',
                'GOW3_ASYNC_SHADERS': '1',
            },
        )


    def test_generated_resolution_is_an_even_size_nobody_provides(self):
        known = g.RESOLUTIONS + ('3840x1080',)
        self.assertEqual(g.generated_resolution('2256x1504', known), (2256, 1504))
        self.assertEqual(g.generated_resolution('1800p', ('720p',)), (3200, 1800))
        for value in ('1440p', '3440x1440', '3840x1080', '1365x768', 'native', 'abc', '0x0', '1366x', None):
            self.assertIsNone(g.generated_resolution(value, known), value)

    def test_generated_resolution_stops_at_the_largest_size(self):
        self.assertEqual(g.generated_resolution('7680x4320', ()), (7680, 4320))
        for value in ('10240x5760', '7682x4320', '7680x4322', '70000x2000'):
            self.assertIsNone(g.generated_resolution(value, ()), value)

    def test_a_size_name_is_digits_x_digits(self):
        self.assertEqual(g.resolution_size('2256x1504'), (2256, 1504))
        for value in ('+2256x1504', '2_256x1504', '01920x1200', ' 1920x1200', '1920x1200 ', '1920x1200x3',
                      '1920X1200', 'x', '', '0x0', '1٩20x1200', '100000x2000'):
            self.assertIsNone(g.resolution_size(value), repr(value))
        for name in g.RESOLUTIONS:
            self.assertIsNotNone(g.resolution_size(name), name)

    def test_a_size_that_is_no_string_has_no_size(self):
        # A hand-edited state file can hold "render_resolution": 5 or a list.
        for value in (5, None, ['x'], {'x': 1}):
            self.assertIsNone(g.resolution_size(value), repr(value))
            self.assertIsNone(g.generated_resolution(value, ()), repr(value))

    def test_the_render_size_limits_match_the_game_profiles_and_the_renderer(self):
        # The launcher's floor is what the WipEout profiles tell the renderer; God of War III has none.
        import game_profiles
        floor = f'{g.MIN_GENERATED[0]}x{g.MIN_GENERATED[1]}'
        for title in g.GENERATED_TITLES:
            self.assertEqual(game_profiles.PROFILES[title]['env']['GOW3_MIN_RENDER_SIZE'], floor, title)
        self.assertEqual(set(g.GENERATED_TITLES), {'CUSA07671', 'CUSA05670'})
        self.assertNotIn('GOW3_MIN_RENDER_SIZE', game_profiles.PROFILES['CUSA01623'].get('env', {}))
        self.assertNotIn('GOW3_MIN_RENDER_SIZE', game_profiles.DEFAULT.get('env', {}))
        # The renderer's ceiling (scene_resolution.h MaxGuest) is the largest generated size.
        header = (paths.ROOT / 'gpu/shadps4/video_core/renderer_vulkan/scene_resolution.h').read_text(encoding='utf-8')
        match = re.search(r'\bMaxGuest\s*\{\s*(\d+)\s*,\s*(\d+)\s*\}', header)
        self.assertIsNotNone(match)
        self.assertEqual((int(match[1]), int(match[2])), g.MAX_GENERATED)

    def test_generated_resolution_starts_at_the_games_own_size(self):
        for value in ('1280x800', '1366x768', '1680x1050', '1920x1000', '1800x1200'):
            self.assertIsNone(g.generated_resolution(value, ()), value)
        self.assertEqual(g.generated_resolution('1920x1200', ()), (1920, 1200))
        self.assertEqual(g.generated_resolution('2256x1504', ()), (2256, 1504))

    def test_size_name_prefers_a_built_in_name(self):
        self.assertEqual(g.size_name(2560, 1440), '1440p')
        self.assertEqual(g.size_name(1920, 1080), 'native')
        self.assertEqual(g.size_name(3200, 1800), '1800p')
        self.assertEqual(g.size_name(3840, 2160), '4K')
        self.assertEqual(g.size_name(2256, 1504), '2256x1504')

    def test_scaled_size_is_even(self):
        self.assertEqual(g.scaled_size((2560, 1440), 100), (2560, 1440))
        self.assertEqual(g.scaled_size((2560, 1440), 150), (3840, 2160))
        self.assertEqual(g.scaled_size((2256, 1504), 125), (2820, 1880))
        self.assertEqual(g.scaled_size((2256, 1504), 150), (3384, 2256))
        self.assertEqual(g.scaled_size((1366, 768), 125), (1708, 960))
        for size in ((2560, 1440), (2256, 1504), (1366, 768), (3440, 1440)):
            for percent in g.RENDER_SCALES:
                width, height = g.scaled_size(size, percent)
                self.assertEqual((width % 2, height % 2), (0, 0), (size, percent))

    def test_render_scales_use_the_hand_made_size_when_there_is_one(self):
        self.assertEqual(g.render_scales_for((2560, 1440)),
                         [(100, '1440p'), (125, '1800p'), (150, '4K'), (200, '5120x2880')])
        self.assertEqual(g.render_scales_for((2561, 1441)), g.render_scales_for((2560, 1440)))

    def test_render_scales_stay_between_the_floor_and_the_cap(self):
        self.assertEqual(g.render_scales_for((1280, 800)), [(150, '1920x1200'), (200, '2560x1600')])
        self.assertEqual(g.render_scales_for((3840, 2160)),
                         [(100, '4K'), (125, '4800x2700'), (150, '5760x3240'), (200, '7680x4320')])
        self.assertEqual([p for p, _n in g.render_scales_for((5120, 2880))], [100, 125, 150])
        self.assertEqual(g.render_scales_for((0, 0)), [])
        self.assertEqual(g.MAX_GENERATED, (7680, 4320))

    def test_render_scale_of_compares_by_size(self):
        self.assertEqual(g.render_scale_of((2560, 1440), '4K'), 150)
        self.assertEqual(g.render_scale_of((2560, 1440), '1440p'), 100)
        self.assertEqual(g.render_scale_of((2560, 1440), '2560x1440'), 100)
        self.assertEqual(g.render_scale_of((2560, 1440), '1800p'), 125)
        self.assertIsNone(g.render_scale_of((2560, 1440), 'native'))
        self.assertIsNone(g.render_scale_of((2560, 1440), None))
        self.assertIsNone(g.render_scale_of((0, 0), '1440p'))


class WindowSizeTest(unittest.TestCase):
    def test_the_game_knows_the_launcher_sizes(self):
        """gpu/shim/gow3_graphics.h ResolutionSizes, which sizes the window, matches SIZES."""
        header = (paths.ROOT / 'gpu' / 'shim' / 'gow3_graphics.h').read_text(encoding='utf-8')
        found = {name: (int(w), int(h)) for name, w, h in re.findall(r'\{"(\w+)", \{(\d+), (\d+)\}\}', header)}
        self.assertEqual(found, g.SIZES)



class AspectRatioTest(unittest.TestCase):
    """The launcher's Aspect ratio filter: the render resolutions of one aspect ratio."""

    def test_aspect_of_a_resolution(self):
        self.assertEqual([g.aspect_of(r) for r in ('native', '720p', '1360x768', '1280x800', '3440x1440',
                                                   '5120x1440', '1280x960', '5120x2160')],
                         ['16:9', '16:9', '16:9', '16:10', '21:9', '32:9', '4:3', '21:9'])
        self.assertEqual(g.aspect_of('1280x1024'), '5:4')  # a mod's size outside the named ones

    def test_aspects_in_a_fixed_order_with_the_others_last(self):
        self.assertEqual(g.aspects(g.RESOLUTIONS), ['16:9', '16:10', '21:9', '32:9', '4:3'])
        self.assertEqual(g.aspects(g.resolutions(('1280x1024',))), ['16:9', '16:10', '21:9', '32:9', '4:3', '5:4'])

    def test_resolutions_of_an_aspect_smallest_first_native_on_top(self):
        self.assertEqual(g.resolutions_for('16:9', g.RESOLUTIONS),
                         ['native', '480p', '720p', '1360x768', '900p', '1440p', '1800p', '4K', '5120x2880',
                          '7680x4320'])
        self.assertEqual(g.resolutions_for('16:10', g.RESOLUTIONS), ['1280x800', '1440x900', '1920x1200', '2560x1600'])
        self.assertEqual(g.resolutions_for('4:3', g.RESOLUTIONS),
                         ['1280x960', '1440x1080', '1920x1440', '2048x1536', '2560x1920'])

    def test_switching_aspect_keeps_the_closest_size(self):
        self.assertEqual(g.closest_resolution('16:10', '1440p', g.RESOLUTIONS), '2560x1600')
        self.assertEqual(g.closest_resolution('4:3', 'native', g.RESOLUTIONS), '1440x1080')
        self.assertEqual(g.closest_resolution('16:9', '1280x800', g.RESOLUTIONS), '1360x768')
        self.assertEqual(g.closest_resolution('16:10', '1440x900', g.RESOLUTIONS), '1440x900')  # already of it

    def test_labels_without_the_aspect_ratio_for_the_filtered_list(self):
        self.assertEqual(g.resolution_label('1800p', aspect=False), '1800p · 3200 × 1800')
        self.assertEqual(g.resolution_label('3440x1440', aspect=False), '3440 × 1440 (beta)')
        self.assertEqual(g.resolution_label('1800p'), '1800p · 3200 × 1800 · 16:9')  # elsewhere it stays

if __name__ == '__main__':
    unittest.main()
