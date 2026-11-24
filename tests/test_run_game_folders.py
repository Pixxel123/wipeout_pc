from paths import ROOT
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(ROOT))
import run


class RunGameFoldersTests(unittest.TestCase):
    def test_game_folder_exported_absolute(self):
        with tempfile.TemporaryDirectory() as folder:
            old = os.getcwd()
            os.chdir(folder)
            try:
                env = {}
                run.export_game_folders(env, Path('CUSA07671'))
                self.assertEqual(env, {'GOW3_GAME_DIR': os.path.join(os.getcwd(), 'CUSA07671')})
            finally:
                os.chdir(old)

    def test_update_folder_exported_absolute_only_when_set(self):
        env = {'GOW3_PATCH_DIR': 'update'}
        run.export_game_folders(env, Path('/games/CUSA07671'))
        self.assertEqual(env['GOW3_GAME_DIR'], os.path.abspath('/games/CUSA07671'))
        self.assertEqual(env['GOW3_PATCH_DIR'], os.path.abspath('update'))
        env = {'GOW3_PATCH_DIR': ''}
        run.export_game_folders(env, Path('/games/CUSA07671'))
        self.assertEqual(env['GOW3_PATCH_DIR'], '')


if __name__ == '__main__':
    unittest.main()
