import tempfile
import unittest
from pathlib import Path

from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import controller_row
import psarc
from psarc_fixtures import archive as fixture

# The shipped layout's two switched-off sliders, byte for byte (data09.psarc), 414 and 411 bytes.
P1 = (b'<aSlider name="Thrust Sensitivity" global="Thrust Sensitivity" focus="true" save="true" default="30%" '
      b'delay="0.5">\r\n              <Values idstring="OPT_CTRL_ACC_SENS" font="default" scale="0.8" x="0" '
      b'y="780" color="FEGlobals->TextColor" arrowcolor="FEGlobals->TextColor" gap="0" minSlide="10" '
      b'maxSlide="100" incSlide="10" txtSlide="%" sideWidth="790" rightmin="700" rightmax="805"></Values>'
      b'\r\n            </aSlider>')
P2 = (b'<aSlider name="Thrust Sensitivity" global="Thrust Sensitivity" focus="true" save="true" default="30%" '
      b'delay="0.5" InputPad="1">\r\n      <Values idstring="OPT_CTRL_ACC_SENS" font="default" scale="0.8" '
      b'x="0" y="780" color="FEGlobals->TextColor" arrowcolor="FEGlobals->TextColor" gap="0" minSlide="10" '
      b'maxSlide="100" incSlide="10" txtSlide="%" sideWidth="790" rightmin="700" rightmax="805"></Values>'
      b'\r\n    </aSlider>')
LAYOUT = (b'<Screen type="Controls" name="Controls">\r\n  ' + P1 + b'\r\n</Screen>\r\n'
          b'<Screen type="Controls" name="ControlsP2">\r\n  ' + P2 + b'\r\n</Screen>\r\n')


class RowTests(unittest.TestCase):
    def test_fixture_is_the_shipped_layout(self):
        self.assertEqual((len(P1), len(P2)), (414, 411))

    def test_both_sliders_become_rows_in_the_same_bytes(self):
        new = controller_row.add_row(LAYOUT)
        self.assertEqual(len(new), len(LAYOUT))
        self.assertNotIn(b'aSlider', new)
        first, second = new.index(b'name="Pad Style P1"'), new.index(b'name="Pad Style P2"')
        self.assertLess(first, second)
        self.assertIn(b'global="Pad Style P2" focus="true" save="true" default="playstation" delay="0.5" '
                      b'InputPad="1">', new)
        self.assertEqual(new.count(b'<Entry idstring="FE_DEFAULT" value="playstation"></Entry>'), 2)
        self.assertEqual(new.count(b'<Entry idstring="XBOX" value="xbox"></Entry>'), 2)
        self.assertEqual(new.count(b'idstring="CONTROLLER TYPE"'), 2)
        self.assertTrue(new.startswith(b'<Screen type="Controls" name="Controls">'))

    def test_a_layout_without_the_sliders_is_unchanged(self):
        xml = b'<Screen name="Controls"></Screen>'
        self.assertEqual(controller_row.add_row(xml), xml)

    def test_a_layout_with_only_one_slider_is_refused(self):
        with self.assertRaises(ValueError):
            controller_row.add_row(P1)

    def test_changes_patch_the_newest_layout(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'data09.psarc'
            fixture(path, {controller_row.LAYOUT: LAYOUT}, packed=False)
            found = controller_row.changes(folder, [(path.name, path)])
            self.assertEqual(list(found), ['data09.psarc'])
            raw = bytearray(path.read_bytes())
            for offset, original, new in found['data09.psarc']:
                self.assertEqual(raw[offset:offset + len(original)], original)
                raw[offset:offset + len(new)] = new
            patched = Path(folder) / 'patched.psarc'
            patched.write_bytes(raw)
            with psarc.Archive(patched) as a:
                self.assertEqual(a.read(a.find(controller_row.LAYOUT)), controller_row.add_row(LAYOUT))


if __name__ == '__main__':
    unittest.main()
