import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

from paths import ROOT

sys.path.insert(0, str(ROOT))
import run


class RunArchiveTests(unittest.TestCase):
    def test_archives_kept_out_of_the_view_are_passed_by_their_game_names(self):
        with tempfile.TemporaryDirectory() as folder:
            view, base = Path(folder) / 'view', Path(folder) / 'base'
            for directory in (view, base, view / 'sub'):
                directory.mkdir()
            for path in (view / 'data09.psarc', view / 'eboot.bin', base / 'data08.psarc', base / 'other.psarc'):
                path.write_bytes(b'')
            unlinked = {str(view / 'data08.psarc'): base / 'data08.psarc',
                        str(view / 'sub' / 'other.psarc'): base / 'other.psarc',  # not in the game folder itself
                        str(view / 'movie.mp4'): base / 'movie.mp4'}
            self.assertEqual(run.archive_args(view, unlinked, view),
                             ['--archive', 'data08.psarc', str(base / 'data08.psarc'),
                              '--archive', 'data09.psarc', str(view / 'data09.psarc')])
            self.assertEqual(run.archive_args(base, {}, None),
                             ['--archive', 'data08.psarc', str(base / 'data08.psarc'),
                              '--archive', 'other.psarc', str(base / 'other.psarc')])

    def test_saved_controller_type_wins_over_the_launcher(self):
        with tempfile.TemporaryDirectory() as folder:
            with mock.patch('pad_style_save.saved_style', return_value='xbox'):
                self.assertEqual(run.launch_style({'GOW3_PAD_STYLE': 'playstation'}, folder, 'CUSA07671'), 'xbox')
            with mock.patch('pad_style_save.saved_style', return_value=None):
                self.assertEqual(run.launch_style({'GOW3_PAD_STYLE': 'nintendo'}, folder, 'CUSA07671'), 'nintendo')
                self.assertEqual(run.launch_style({}, folder, 'CUSA07671'), 'playstation')
            with mock.patch('pad_style_save.saved_style', return_value='motion off'):
                self.assertEqual(run.launch_style({'GOW3_PAD_STYLE': 'xbox'}, folder, 'CUSA07671'), 'xbox')

    def test_a_saved_auto_starts_with_the_connected_gamepads_style(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch('pad_style_save.saved_style', return_value='auto'):
            pad = 'Gamepad: type=ps5 south=cross east=circle west=square north=triangle name=DualSense\n'
            with mock.patch('run.subprocess.run', return_value=mock.Mock(stdout=pad)) as probe, \
                    mock.patch('builtins.print'):
                self.assertEqual(run.launch_style({'GOW3_PAD_STYLE': 'xbox'}, folder, 'CUSA07671', 'probe'),
                                 'playstation')
            self.assertEqual(probe.call_args[0][0], ['probe', '--gamepad'])
            # No gamepad, or the probe fails: the launcher's controller type, else PlayStation.
            with mock.patch('run.subprocess.run', return_value=mock.Mock(stdout='Gamepad: none\n')):
                self.assertEqual(run.launch_style({'GOW3_PAD_STYLE': 'xbox'}, folder, 'CUSA07671', 'probe'), 'xbox')
            with mock.patch('run.subprocess.run', side_effect=OSError('no probe')):
                self.assertEqual(run.launch_style({}, folder, 'CUSA07671', 'probe'), 'playstation')
            with mock.patch('run.subprocess.run', side_effect=AssertionError('asked')):
                self.assertEqual(run.launch_style({'GOW3_PAD_STYLE': 'nintendo'}, folder, 'CUSA07671'), 'nintendo')
        with tempfile.TemporaryDirectory() as folder, mock.patch('pad_style_save.saved_style', return_value='xbox'), \
                mock.patch('run.subprocess.run', side_effect=AssertionError('asked')):
            self.assertEqual(run.launch_style({}, folder, 'CUSA07671', 'probe'), 'xbox')  # only AUTO asks

    def test_the_rows_need_their_settings_in_the_save(self):
        self.assertEqual(run.row_scripts('playstation', True), [('controller_row.py', [])])
        self.assertEqual(run.row_scripts('nintendo', True), [('controller_row.py', [])])
        self.assertEqual(run.row_scripts('xbox', True), [('xbox_controls.py', [])])
        # Without them: no rows, but the Xbox picture is still drawn.
        self.assertEqual(run.row_scripts('playstation', False), [])
        self.assertEqual(run.row_scripts('nintendo', False), [])
        self.assertEqual(run.row_scripts('xbox', False), [('xbox_controls.py', ['--no-row'])])


if __name__ == '__main__':
    unittest.main()
