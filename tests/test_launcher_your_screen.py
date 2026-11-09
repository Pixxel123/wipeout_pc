"""The launcher offers the monitor's size as a render resolution ("Your screen"), like a PC game: a
hand-made size keeps its name, any other even size gets a generated patch at start (beta)."""
import sys
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import gow3_launcher_win as launcher  # noqa: E402
import test_launcher_diagnostics_layout as layout  # noqa: E402


class YourScreenTest(unittest.TestCase):
    def test_hand_made_sizes_keep_their_name(self):
        self.assertEqual(launcher.your_screen_choice((2560, 1440)), ('1440p', ('Your screen · 2560 × 1440 · 16:9',)))
        self.assertEqual(launcher.your_screen_choice((2561, 1441)), ('1440p', ('Your screen · 2560 × 1440 · 16:9',)))
        self.assertEqual(launcher.your_screen_choice((1920, 1080)), ('native', ('Your screen · 1920 × 1080 · 16:9',)))
        self.assertEqual(launcher.your_screen_choice((1280, 800)), ('1280x800', ('Your screen · 1280 × 800 · 16:10',)))

    def test_other_sizes_are_generated_and_beta(self):
        self.assertEqual(launcher.your_screen_choice((2256, 1504)), ('2256x1504', ('Your screen · 2256 × 1504 · 3:2 (beta)',)))
        self.assertEqual(launcher.your_screen_choice((1366, 768)), ('1366x768', ('Your screen · 1366 × 768 · 16:9 (beta)',)))

    def test_too_small_or_unknown_monitors_offer_nothing(self):
        self.assertIsNone(launcher.your_screen_choice((1024, 600)))
        self.assertIsNone(launcher.your_screen_choice((0, 0)))

    def test_options_put_your_screen_first_without_duplicates(self):
        options = launcher.resolution_choices_for(['native', '1440p', '3440x1440'], (2560, 1440), saved='3440x1440')
        self.assertEqual(options[0], ('1440p', ('Your screen · 2560 × 1440 · 16:9',)))
        self.assertEqual([v for v, _t in options], ['1440p', 'native', '3440x1440'])
        options = launcher.resolution_choices_for(['native', '1440p'], (2256, 1504), saved='1440p')
        self.assertEqual([v for v, _t in options], ['2256x1504', 'native', '1440p'])

    def test_a_saved_generated_size_stays_selectable(self):
        options = launcher.resolution_choices_for(['native', '1440p'], (1920, 1080), saved='2256x1504')
        self.assertEqual([v for v, _t in options], ['native', '1440p', '2256x1504'])
        self.assertEqual(dict(options)['2256x1504'], ('2256 × 1504 · 3:2 (beta)',))

    def test_default_is_your_screen_only_without_a_saved_value(self):
        self.assertEqual(launcher.default_render_resolution({}, (2256, 1504)), '2256x1504')
        self.assertEqual(launcher.default_render_resolution({'render_resolution': '1440p'}, (2256, 1504)), '1440p')
        self.assertEqual(launcher.default_render_resolution({}, (0, 0)), 'native')


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

    def test_a_game_without_generated_sizes_changes_nothing(self):
        self.assertIsNone(launcher.fresh_render_resolution([], (2256, 1504), False))

    def test_native_needs_no_line_in_the_file(self):
        self.assertIsNone(launcher.fresh_render_resolution([], (1920, 1080), True))


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
        self.assertFalse(app.offers_your_screen())
        self.assertEqual(app.game_resolution_options(), launcher.RESOLUTION_CHOICES)
        self.assertEqual(app.resolution_options(), launcher.RESOLUTION_CHOICES)

    def test_wipeout_lists_start_with_the_screen(self):
        app = self.app('CUSA07671')
        self.assertEqual(self.values(app.game_resolution_options())[:2], ['2256x1504', 'native'])
        self.assertEqual(self.values(app.resolution_options())[:2], ['2256x1504', 'native'])

    def test_no_game_lists_every_built_in_size_after_the_screen(self):
        app = self.app(None, folder='')
        builtin = self.values(launcher.RESOLUTION_CHOICES)
        self.assertEqual(self.values(app.game_resolution_options()), ['2256x1504'] + builtin)

    def test_a_saved_size_is_kept_in_the_wipeout_list(self):
        app = self.app('CUSA07671', saved='1800p')  # built in, but WipEout has no patch of its own for it
        self.assertEqual(self.values(app.game_resolution_options())[-1], '1800p')


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
            self.assertEqual(app.game_resolution_options()[0][1], ('Your screen · 2256 × 1504 · 3:2 (beta)',))


if __name__ == '__main__':
    unittest.main()
