"""The launcher offers the monitor's size as a render resolution (the "Native" row), like a PC game: a
hand-made size keeps its name, any other even size from 1920x1080 up gets a generated patch at start (beta)."""
import sys
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import gow3_launcher_win as launcher  # noqa: E402
import test_launcher_diagnostics_layout as layout  # noqa: E402


WIPEOUT_SIZES = ['native', '720p', '900p', '1440p', '4K', '2560x1080', '3440x1440', '3840x1600', '5120x1440', '1920x1200',
                 '2560x1600', '2880x1800', '1920x1440', '5120x2160']


class YourScreenTest(unittest.TestCase):
    def test_hand_made_sizes_keep_their_name(self):
        self.assertEqual(launcher.your_screen_choice((2560, 1440)), ('1440p', ('2560 × 1440 (Native)',)))
        self.assertEqual(launcher.your_screen_choice((2561, 1441)), ('1440p', ('2560 × 1440 (Native)',)))
        self.assertEqual(launcher.your_screen_choice((1920, 1080)), ('native', ('1920 × 1080 (Native)',)))
        self.assertEqual(launcher.your_screen_choice((1920, 1200)), ('1920x1200', ('1920 × 1200 (Native)',)))

    def test_the_native_mark_is_translated(self):
        with mock.patch.object(launcher, 'LANG', 'pt'):
            self.assertEqual(launcher.your_screen_choice((2560, 1440))[1], ('2560 × 1440 (Nativa)',))

    def test_other_sizes_are_generated_without_a_beta_mark(self):
        self.assertEqual(launcher.your_screen_choice((2256, 1504)), ('2256x1504', ('2256 × 1504 (Native)',)))
        self.assertEqual(launcher.your_screen_choice((2560, 1200)), ('2560x1200', ('2560 × 1200 (Native)',)))

    def test_too_small_or_unknown_monitors_offer_nothing(self):
        self.assertIsNone(launcher.your_screen_choice((1024, 600)))
        self.assertIsNone(launcher.your_screen_choice((0, 0)))

    def test_below_the_games_own_size_offers_nothing(self):
        for size in ((1280, 800), (1366, 768), (1680, 1050), (1920, 1000), (1800, 1200)):
            self.assertIsNone(launcher.your_screen_choice(size), size)

    def test_options_put_your_screen_first_without_duplicates(self):
        options = launcher.resolution_choices_for(['native', '1440p', '3440x1440'], (2560, 1440), saved='3440x1440')
        self.assertEqual(options[0], ('1440p', ('2560 × 1440 (Native)',)))
        self.assertEqual([v for v, _t in options], ['1440p', '3440x1440', 'native'])
        options = launcher.resolution_choices_for(['native', '1440p'], (2256, 1504), saved='1440p')
        self.assertEqual([v for v, _t in options], ['2256x1504', '1440p', 'native'])

    def test_a_saved_generated_size_stays_selectable(self):
        options = launcher.resolution_choices_for(['native', '720p'], (1920, 1080), saved='2256x1504')
        self.assertEqual([v for v, _t in options], ['native', '2256x1504'])
        self.assertEqual(dict(options)['2256x1504'], ('2256 × 1504',))

    def test_a_saved_size_below_the_floor_is_not_kept(self):
        for saved in ('1280x800', '1366x768'):
            options = launcher.resolution_choices_for(['native', '720p'], (1920, 1080), saved=saved)
            self.assertEqual([v for v, _t in options], ['native'], saved)

    def test_rows_are_plain_sizes(self):
        options = dict(launcher.resolution_choices_for(['native', '2560x1080', '1440p'], (2560, 1440)))
        self.assertEqual(options, {'1440p': ('2560 × 1440 (Native)',), 'native': ('1920 × 1080',),
                                   '2560x1080': ('2560 × 1080',)})

    def test_a_1080p_monitor_has_one_native_row(self):
        options = launcher.resolution_choices_for(['native', '1440p'], (1920, 1080), saved='1440p')
        self.assertEqual(options, [('native', ('1920 × 1080 (Native)',)), ('1440p', ('2560 × 1440',))])

    def test_a_1080p_monitor_lists_exactly_one_row(self):
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (1920, 1080))
        self.assertEqual(options, [('native', ('1920 × 1080 (Native)',))])

    def test_exactly_one_row_says_native(self):
        for size in ((2560, 1440), (2256, 1504), (1920, 1080)):
            options = launcher.resolution_choices_for(['native', '720p', '1440p'], size)
            self.assertEqual(len([t for _v, (t,) in options if t.endswith('(Native)')]), 1, size)

    def test_only_sizes_that_fit_the_monitor_are_listed(self):
        options = launcher.resolution_choices_for(['native', '720p', '900p', '1440p', '4K', '3440x1440'], (2560, 1440))
        self.assertEqual([v for v, _t in options], ['1440p', 'native'])

    def test_a_size_too_tall_for_the_monitor_is_dropped(self):
        options = launcher.resolution_choices_for(['native', '2560x1080', '2560x1600'], (2560, 1440))
        self.assertEqual([v for v, _t in options], ['1440p', '2560x1080', 'native'])

    def test_sizes_below_the_games_own_are_dropped(self):
        for size in ((2560, 1440), (1280, 800)):
            values = [v for v, _t in launcher.resolution_choices_for(['native', '720p', '900p', '1280x800', '1360x768'], size)]
            self.assertEqual([v for v in values if v in ('720p', '900p', '1280x800', '1360x768')], [], size)
            self.assertIn('native', values, size)

    def test_a_saved_size_below_the_games_own_stays_listed(self):
        options = launcher.resolution_choices_for(['native', '720p', '900p'], (2560, 1440), saved='720p')
        self.assertEqual(options, [('1440p', ('2560 × 1440 (Native)',)), ('native', ('1920 × 1080',)), ('720p', ('1280 × 720',))])

    def test_rows_after_the_native_one_are_largest_first(self):
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (3840, 2160))
        self.assertEqual([v for v, _t in options], ['4K', '3840x1600', '3440x1440', '2880x1800', '2560x1600', '1440p', '2560x1080',
                                                    '1920x1440', '1920x1200', 'native'])

    def test_a_name_without_a_size_stays_at_the_end(self):
        options = launcher.resolution_choices_for(['custom', 'native', 'other', '2560x1080'], (2560, 1440))
        self.assertEqual([v for v, _t in options], ['1440p', '2560x1080', 'native', 'custom', 'other'])

    def test_a_saved_mod_size_has_no_row_of_its_own(self):
        options = launcher.resolution_choices_for(['native'], (2560, 1440), saved='3840x1080', mods=['3840x1080'])
        self.assertEqual([v for v, _t in options], ['1440p', 'native'])

    def test_a_saved_size_that_does_not_fit_stays_listed(self):
        options = launcher.resolution_choices_for(['native', '1440p', '4K'], (2560, 1440), saved='4K')
        self.assertEqual([v for v, _t in options], ['1440p', '4K', 'native'])
        self.assertEqual(dict(options)['4K'], ('3840 × 2160',))
        options = launcher.resolution_choices_for(['native', '1440p'], (2560, 1440), saved='2880x1920')
        self.assertEqual(options[1], ('2880x1920', ('2880 × 1920',)))

    def test_no_generated_row_is_marked_beta(self):
        for size, saved in (((2256, 1504), None), ((1920, 1080), '2256x1504')):
            labels = [t for _v, (t,) in launcher.resolution_choices_for(['native', '1440p'], size, saved=saved)]
            self.assertFalse([t for t in labels if 'beta' in t], size)

    def test_without_a_monitor_row_larger_sizes_stay(self):
        names = ['native', '720p', '1280x800', '1440p', '4K', '3440x1440']
        for size in ((1280, 800), (0, 0)):
            options = launcher.resolution_choices_for(names, size)
            self.assertEqual([v for v, _t in options], ['4K', '3440x1440', '1440p', 'native'], size)
            self.assertFalse([t for _v, (t,) in options if '(Native)' in t], size)
            self.assertEqual(dict(options)['native'], ('1920 × 1080',), size)

    def test_default_is_your_screen_only_without_a_saved_value(self):
        self.assertEqual(launcher.default_render_resolution({}, (2256, 1504)), '2256x1504')
        self.assertEqual(launcher.default_render_resolution({'render_resolution': '1440p'}, (2256, 1504)), '1440p')
        self.assertEqual(launcher.default_render_resolution({}, (0, 0)), 'native')
        self.assertEqual(launcher.default_render_resolution({}, (1280, 800)), 'native')


class StaleResolutionTest(unittest.TestCase):
    """What a saved render resolution that is not on the list becomes."""

    def test_a_deck_falls_to_native_docked_or_not(self):
        for size in ((3840, 2160), (2560, 1440), (1280, 800)):
            options = launcher.resolution_choices_for(WIPEOUT_SIZES, size)
            self.assertEqual(launcher.stale_render_resolution(options, size, True), 'native', size)

    def test_a_pc_falls_to_its_monitors_row(self):
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (3840, 2160))
        self.assertEqual(launcher.stale_render_resolution(options, (3840, 2160), False), '4K')

    def test_without_a_monitor_row_the_largest_size_is_not_picked(self):
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (1366, 768))
        self.assertEqual(options[0][0], '5120x2160')
        self.assertEqual(launcher.stale_render_resolution(options, (1366, 768), False), 'native')

    def test_a_list_without_native_keeps_its_first_row(self):
        options = [('720p', ('1280 × 720',)), ('900p', ('1600 × 900',))]
        self.assertEqual(launcher.stale_render_resolution(options, (1280, 800), True), '720p')


class MonitorSizeTest(unittest.TestCase):
    def root(self):
        root = mock.Mock()
        root.tk.call.return_value = 'x11'
        root.winfo_screenwidth.return_value, root.winfo_screenheight.return_value = 1024, 768
        return root

    def test_x11_reuses_the_monitor_already_read(self):
        with mock.patch.object(launcher, 'monitor_under_pointer') as read:
            self.assertEqual(launcher.monitor_size(self.root(), (100, 0, 2560, 1440)), (2560, 1440))
        read.assert_not_called()

    def test_an_unknown_monitor_uses_the_screen_size(self):
        with mock.patch.object(launcher, 'monitor_under_pointer') as read:
            self.assertEqual(launcher.monitor_size(self.root(), (0, 0, 0, 0)), (1024, 768))
        read.assert_not_called()


class FreshInstallTest(unittest.TestCase):
    """What a new gow3.ini starts with: None leaves the file alone, so the game stays at native."""

    def test_no_file_starts_at_the_screen(self):
        self.assertEqual(launcher.fresh_render_resolution([], (2256, 1504), True), '2256x1504')
        self.assertEqual(launcher.fresh_render_resolution([], (2560, 1440), True), '1440p')

    def test_a_file_with_other_keys_is_an_existing_install(self):
        self.assertIsNone(launcher.fresh_render_resolution(['upscaler=fsr4'], (2256, 1504), True))
        self.assertIsNone(launcher.fresh_render_resolution(['# gow3 settings'], (2256, 1504), True))

    def test_a_monitor_below_the_minimum_changes_nothing(self):
        self.assertIsNone(launcher.fresh_render_resolution([], (1024, 600), True))
        self.assertIsNone(launcher.fresh_render_resolution([], (0, 0), True))
        self.assertIsNone(launcher.fresh_render_resolution([], (1280, 800), True))
        self.assertIsNone(launcher.fresh_render_resolution([], (1366, 768), True))

    def test_a_game_without_generated_sizes_changes_nothing(self):
        self.assertIsNone(launcher.fresh_render_resolution([], (2256, 1504), False))

    def test_native_needs_no_line_in_the_file(self):
        self.assertIsNone(launcher.fresh_render_resolution([], (1920, 1080), True))

    def test_a_deck_keeps_its_default_docked_or_not(self):
        for size in ((1280, 800), (2256, 1504), (2560, 1440), (3840, 2160)):
            self.assertIsNone(launcher.fresh_render_resolution([], size, True, deck=True), size)

    def test_any_other_pc_starts_at_the_screen_at_any_size(self):
        self.assertEqual(launcher.fresh_render_resolution([], (3840, 2160), True, deck=False), '4K')


QHD_MODES = [(2560, 1440), (1920, 1080), (1680, 1050), (1280, 720)]


class MonitorModesTest(unittest.TestCase):
    def test_the_list_is_the_monitors_modes_from_the_floor_up(self):
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (2560, 1440), modes=QHD_MODES)
        self.assertEqual(options, [('1440p', ('2560 × 1440 (Native)',)), ('native', ('1920 × 1080',))])

    def test_modes_above_the_desktop_size_stay(self):
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (2560, 1440), modes=[(3840, 2160), (2560, 1440), (1920, 1080)])
        self.assertEqual([v for v, _t in options], ['1440p', '4K', 'native'])
        self.assertEqual(dict(options)['4K'], ('3840 × 2160',))

    def test_the_deck_panel_lists_only_the_games_own_size(self):
        self.assertEqual(launcher.resolution_choices_for(WIPEOUT_SIZES, (1280, 800), modes=[(1280, 800), (160, 256)]),
                         [('native', ('1920 × 1080',))])

    def test_odd_small_and_absent_sizes_are_not_listed(self):
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (3840, 2160),
                                                  modes=[(3840, 2160), (2560, 1440), (1920, 1080), (1366, 768), (1280, 720)])
        self.assertEqual([v for v, _t in options], ['4K', '1440p', 'native'])  # no 3440x1440: the monitor has no such mode

    def test_a_saved_size_and_mods_are_kept_once(self):
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (2560, 1440), saved='2256x1504', modes=QHD_MODES)
        self.assertEqual([v for v, _t in options], ['1440p', '2256x1504', 'native'])
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (2560, 1440), saved='3840x1080', mods=['3840x1080'], modes=QHD_MODES)
        self.assertEqual([v for v, _t in options], ['1440p', 'native'])

    def test_a_saved_size_below_the_floor_stays_listed(self):
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (2560, 1440), saved='720p', modes=QHD_MODES)
        self.assertEqual(options, [('1440p', ('2560 × 1440 (Native)',)), ('native', ('1920 × 1080',)), ('720p', ('1280 × 720',))])
        options = launcher.resolution_choices_for(WIPEOUT_SIZES, (2560, 1440), saved='1280x800', modes=QHD_MODES)
        self.assertEqual(options[-1], ('1280x800', ('1280 × 800',)))
        self.assertEqual([v for v, _t in options], ['1440p', 'native', '1280x800'])

    def test_without_modes_the_list_is_todays(self):
        self.assertEqual(launcher.resolution_choices_for(WIPEOUT_SIZES, (2560, 1440), modes=()),
                         launcher.resolution_choices_for(WIPEOUT_SIZES, (2560, 1440)))
        self.assertEqual([v for v, _t in launcher.resolution_choices_for(WIPEOUT_SIZES, (2560, 1440))],
                         ['1440p', '2560x1080', '1920x1440', '1920x1200', 'native'])


class Var:
    def __init__(self, value):
        self.value = value

    def get(self):
        return self.value


class GameGateTest(unittest.TestCase):
    """Only WipEout's binary takes a generated size: God of War III keeps today's lists."""

    def app(self, title_id, saved='native', folder='game', var=True):
        app = launcher.Launcher.__new__(launcher.Launcher)
        app.tk, app.app = None, {'game_dir': folder}
        app.vars = {'game_dir': Var(folder), 'mods_enabled': Var(False)} if var else {}
        app.ini = {'render_resolution': saved}
        app.screen_size = (2256, 1504)
        app.display_modes = []
        self.addCleanup(mock.patch.stopall)
        mock.patch('patches.game_title_id', return_value=title_id).start()
        return app

    def values(self, options):
        return [v for v, _t in options]

    def test_wipeout_and_no_game_offer_it(self):
        for title_id in ('CUSA07671', 'CUSA05670', None):
            with self.subTest(title_id=title_id):
                self.assertTrue(self.app(title_id).offers_your_screen())
        self.assertTrue(self.app(None, folder='').offers_your_screen())

    def test_the_game_is_read_from_the_settings_before_any_variable_exists(self):
        self.assertFalse(self.app('CUSA01623', var=False).offers_your_screen())
        self.assertTrue(self.app('CUSA07671', var=False).offers_your_screen())

    def test_god_of_war_keeps_todays_lists(self):
        app = self.app('CUSA01623', saved='2256x1504')
        app.display_modes = QHD_MODES
        self.assertFalse(app.offers_your_screen())
        self.assertEqual(app.game_resolution_options(), launcher.RESOLUTION_CHOICES)
        self.assertEqual(app.resolution_options(), launcher.RESOLUTION_CHOICES)

    def test_wipeout_lists_the_monitors_modes_when_known(self):
        app = self.app('CUSA07671')
        app.screen_size = (2560, 1440)
        app.display_modes = QHD_MODES
        self.assertEqual(self.values(app.game_resolution_options()), ['1440p', 'native'])
        self.assertEqual(self.values(app.resolution_options()), ['1440p', 'native'])

    def test_the_live_render_resolution_stays_listed(self):
        app = self.app('CUSA07671')
        app.screen_size = (2560, 1440)
        app.display_modes = QHD_MODES
        app.vars['render_resolution'] = Var('4K')  # set live, as the Render scale row does
        self.assertIn('4K', self.values(app.game_resolution_options()))
        self.assertIn('4K', self.values(app.resolution_options()))

    def test_wipeout_lists_start_with_the_screen(self):
        app = self.app('CUSA07671')
        self.assertEqual(self.values(app.game_resolution_options()), ['2256x1504', '1920x1440', '1920x1200', 'native'])
        self.assertEqual(self.values(app.resolution_options()), ['2256x1504', '1920x1440', '1920x1200', 'native'])

    def test_no_game_lists_every_built_in_size_that_takes_effect(self):
        app = self.app(None, folder='')
        app.screen_size = (7680, 4320)  # every built-in size fits
        self.assertEqual(self.values(app.game_resolution_options()),
                         ['7680x4320', '5120x2160', '5120x1440', '4K', '3840x1600', '3440x1440', '1800p', '2560x1920', '2560x1600',
                          '1440p', '2560x1080', '2048x1536', '1920x1440', '1920x1200', 'native'])

    def test_wipeout_lists_only_what_fits(self):
        app = self.app('CUSA07671')
        values = self.values(app.game_resolution_options())
        self.assertIn('native', values)
        for name in ('4K', '3440x1440', '2880x1800'):
            self.assertNotIn(name, values)

    def test_a_size_with_a_patch_is_plain_in_the_wipeout_list(self):
        app = self.app('CUSA07671')
        app.screen_size = (3840, 2160)
        self.assertEqual(dict(app.game_resolution_options())['2880x1800'], ('2880 × 1800',))

    def test_a_saved_size_nobody_lists_has_no_beta_mark(self):
        options = dict(self.app('CUSA07671', saved='2880x1920').game_resolution_options())
        self.assertEqual(options['2880x1920'], ('2880 × 1920',))

    def test_a_mods_size_is_not_beta(self):
        app = self.app('CUSA07671')
        app.vars.update({'mods_enabled': Var(True), 'mods_dir': Var('mods')})
        app.mod_order, app.mod_vars, app.aspect = ['Ultrawide'], {'Ultrawide': Var(True)}, Var('32:9')
        with mock.patch('mods.patch_files', return_value=['u.xml']), \
                mock.patch('patches.resolution_names', return_value=['3840x1080']):
            self.assertEqual(dict(app.resolution_options())['3840x1080'], ('3840 × 1080 · 32:9 (mod)',))
            self.assertEqual(dict(app.aspect_resolution_options())['3840x1080'], ('3840 × 1080 (mod)',))

    def test_a_saved_mods_size_is_listed_once(self):
        app = self.app('CUSA07671', saved='3840x1080')
        app.vars.update({'mods_enabled': Var(True), 'mods_dir': Var('mods')})
        app.mod_order, app.mod_vars = ['Ultrawide'], {'Ultrawide': Var(True)}
        with mock.patch('mods.patch_files', return_value=['u.xml']), \
                mock.patch('patches.resolution_names', return_value=['3840x1080']):
            rows = [t for v, (t,) in app.resolution_options() if v == '3840x1080']
        self.assertEqual(rows, ['3840 × 1080 · 32:9 (mod)'])

    def test_a_saved_size_is_kept_in_the_wipeout_list(self):
        app = self.app('CUSA07671', saved='1800p')  # built in, but WipEout has no patch of its own for it
        self.assertEqual(self.values(app.game_resolution_options()), ['2256x1504', '1800p', '1920x1440', '1920x1200', 'native'])


class FreshWindowTest(unittest.TestCase):
    """A real window over a settings folder without gow3.ini."""
    window = layout.DiagnosticsLayoutTest.window
    SCREEN = (2256, 1504)

    @unittest.skipUnless(layout.tk, 'requires Tk')
    def test_the_default_reaches_the_file_and_the_list(self):
        with self.window('en', 1.0) as (_root, app):
            self.assertEqual(app.var('render_resolution', 'ini').get(), '2256x1504')
            self.assertEqual(launcher.load_ini()[0]['render_resolution'], '2256x1504')
            self.assertIn('render_resolution=2256x1504', launcher.ini_path().read_text(encoding='utf-8'))
            self.assertEqual(app.game_resolution_options()[0][1], ('2256 × 1504 (Native)',))


DECK_XRANDR = """Screen 0: minimum 320 x 200, current 1280 x 800, maximum 16384 x 16384
eDP connected primary 1280x800+0+0 right (normal left inverted right x axis y axis) 60mm x 60mm
   800x1280      60.00*+
   256x160       58.79  
DisplayPort-0 disconnected (normal left inverted right x axis y axis)
"""

DESKTOP_XRANDR = """Screen 0: minimum 8 x 8, current 4480 x 1440, maximum 32767 x 32767
DP-0 connected primary 2560x1440+0+0 (normal left inverted right x axis y axis) 597mm x 336mm
   2560x1440     59.95*+ 143.97   119.88
   1920x1080     60.00    59.94    50.00
   1920x1080i    60.00    59.94
   1680x1050     59.95
   1280x720      60.00    59.94
HDMI-0 connected 1920x1080+2560+0 (normal left inverted right x axis y axis) 527mm x 296mm
   1920x1080     60.00*+
   1366x768      59.79
DP-1 disconnected (normal left inverted right x axis y axis)
"""


class DisplayModesTest(unittest.TestCase):
    def test_a_rotated_panel_is_read_landscape(self):
        self.assertEqual(launcher.xrandr_modes(DECK_XRANDR, (0, 0, 1280, 800)), [(1280, 800), (160, 256)])

    def test_the_pointer_monitor_largest_first_without_rates_or_interlaced(self):
        self.assertEqual(launcher.xrandr_modes(DESKTOP_XRANDR, (0, 0, 2560, 1440)),
                         [(2560, 1440), (1920, 1080), (1680, 1050), (1280, 720)])
        self.assertEqual(launcher.xrandr_modes(DESKTOP_XRANDR, (2560, 0, 1920, 1080)), [(1920, 1080), (1366, 768)])

    def test_unknown_monitor_or_no_output_gives_no_modes(self):
        self.assertEqual(launcher.xrandr_modes(DESKTOP_XRANDR, (0, 0, 0, 0)), [])
        self.assertEqual(launcher.xrandr_modes('', (0, 0, 2560, 1440)), [])
        self.assertEqual(launcher.xrandr_modes(DESKTOP_XRANDR, (0, 0, 3840, 2160)), [])

    def test_windows_modes_dedupe_rates_and_sort(self):
        table = [(1920, 1080), (1280, 720), (2560, 1440), (1920, 1080), (2560, 1440), (1024, 768)]
        self.assertEqual(launcher.windows_modes(mode_at=lambda i: table[i] if i < len(table) else None),
                         [(2560, 1440), (1920, 1080), (1280, 720), (1024, 768)])
        with mock.patch.object(launcher, 'WINDOWS', False):
            self.assertEqual(launcher.windows_modes(), [])

    def test_display_modes_by_platform_without_a_subprocess(self):
        root = mock.Mock()
        root.tk.call.return_value = 'x11'
        with mock.patch.object(launcher.subprocess, 'run', side_effect=AssertionError('no subprocess here')):
            self.assertEqual(launcher.display_modes(root, (0, 0, 2560, 1440), DESKTOP_XRANDR),
                             [(2560, 1440), (1920, 1080), (1680, 1050), (1280, 720)])
            root.tk.call.return_value = 'win32'
            with mock.patch.object(launcher, 'windows_modes', return_value=[(1920, 1080)]):
                self.assertEqual(launcher.display_modes(root, (0, 0, 0, 0), ''), [(1920, 1080)])
            root.tk.call.return_value = 'aqua'
            self.assertEqual(launcher.display_modes(root, (0, 0, 0, 0), ''), [])

    def test_monitor_under_pointer_takes_the_text(self):
        root = mock.Mock()
        root.winfo_pointerxy.return_value = (3000, 100)
        with mock.patch.object(launcher.subprocess, 'run', side_effect=AssertionError('no subprocess here')):
            self.assertEqual(launcher.monitor_under_pointer(root, DESKTOP_XRANDR), (2560, 0, 1920, 1080))


class TracedVar:
    """A Tk StringVar stand-in: get/set and write traces, no display."""
    def __init__(self, value=''):
        self.value, self.traces = value, []

    def get(self):
        return self.value

    def set(self, value):
        self.value = value
        for callback in list(self.traces):
            callback()

    def trace_add(self, _mode, callback):
        self.traces.append(callback)


class RenderScaleTest(unittest.TestCase):
    def app(self, size, resolution):
        import types
        app = launcher.Launcher.__new__(launcher.Launcher)
        app.screen_size = size
        app.display_modes = []
        app.ini = {'render_resolution': resolution}
        app.vars = {'render_resolution': TracedVar(resolution)}
        app.tk = types.SimpleNamespace(StringVar=lambda value='': TracedVar(value))
        return app

    def test_the_row_reads_the_current_scale_and_lists_the_sizes(self):
        app = self.app((2560, 1440), '1440p')
        self.assertEqual(app.render_scale_var().get(), '100')
        self.assertEqual(app.render_scale_options(),
                         [('100', ('100 % · 2560 × 1440',)), ('125', ('125 % · 3200 × 1800',)),
                          ('150', ('150 % · 3840 × 2160',)), ('200', ('200 % · 5120 × 2880',))])

    def test_choosing_a_scale_sets_the_render_resolution(self):
        app = self.app((2560, 1440), '1440p')
        app.render_scale_var().set('150')
        self.assertEqual(app.vars['render_resolution'].get(), '4K')
        app.render_scale_var().set('125')
        self.assertEqual(app.vars['render_resolution'].get(), '1800p')
        app.render_scale_var().set('100')
        self.assertEqual(app.vars['render_resolution'].get(), '1440p')

    def test_a_size_that_is_no_scale_reads_custom_and_is_left_alone(self):
        app = self.app((2560, 1440), '1440p')
        app.render_scale_var()
        app.vars['render_resolution'].set('native')
        self.assertEqual(app.render_scale_var().get(), 'custom')
        self.assertEqual(app.render_scale_options()[-1], ('custom', ('Custom', 'Другое')))
        self.assertEqual(app.vars['render_resolution'].get(), 'native')
        app.vars['render_resolution'].set('4K')
        self.assertEqual(app.render_scale_var().get(), '150')
        self.assertNotIn('custom', [v for v, _t in app.render_scale_options()])

    def test_the_deck_panel_offers_only_scales_that_take_effect(self):
        app = self.app((1280, 800), 'native')
        self.assertEqual([v for v, _t in app.render_scale_options()], ['150', '200', 'custom'])
        app.render_scale_var().set('150')
        self.assertEqual(app.vars['render_resolution'].get(), '1920x1200')

    def test_an_unknown_monitor_has_no_scales(self):
        app = self.app((0, 0), 'native')
        self.assertEqual(launcher.graphics_options.render_scales_for(app.screen_size), [])
        self.assertEqual(app.render_scale_var().get(), 'custom')


class RenderScaleWindowTest(unittest.TestCase):
    """A real window on a 2560 × 1440 monitor: the Render resolution box shows the size the scale row picks."""
    window = layout.DiagnosticsLayoutTest.window
    SCREEN = (2560, 1440)

    @unittest.skipUnless(layout.tk, 'requires Tk')
    def test_the_render_resolution_box_shows_the_scaled_size(self):
        with self.window('en', 1.0) as (root, app):
            app.show('graphics')
            root.update()
            boxes = [w for w in layout.descendants(app.pages['graphics']) if w.winfo_class() == 'TCombobox']
            resolution = next(b for b in boxes if any('Native' in v for v in b.cget('values')))
            self.assertEqual(resolution.get(), '2560 × 1440 (Native)')
            app.render_scale_var().set('150')
            root.update()
            self.assertEqual(app.var('render_resolution', 'ini').get(), '4K')
            self.assertEqual(resolution.get(), '3840 × 2160')


if __name__ == '__main__':
    unittest.main()
