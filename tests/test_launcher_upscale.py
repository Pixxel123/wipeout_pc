"""Temporal upscaling stays parked; spatial upscaling is removed from the launcher."""
import sys
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import gow3_launcher_win as launcher


class UpscalerSettingsTest(unittest.TestCase):
    def test_remaining_choices_have_no_spatial_upscaler(self):
        self.assertEqual([value for value, _text in launcher.UPSCALERS], ['fsr3', 'taa', 'off'])

    def test_removed_controls_do_not_supply_defaults_to_save(self):
        for key in ('fsr1', 'sharpen', 'sharpness'):
            self.assertNotIn(key, launcher.INI_DEFAULTS)

    def test_sharpening_defaults_are_the_games(self):
        # The Graphics page's Sharpening row (rcas, rcas_strength): gow3_settings.h starts RCAS on at 75 %.
        self.assertEqual((launcher.INI_DEFAULTS['rcas'], launcher.INI_DEFAULTS['rcas_strength']), ('1', '75'))
        self.assertIn('rcas', launcher.INI_FLAGS)


if __name__ == '__main__':
    unittest.main()
