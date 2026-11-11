import re
import tempfile
import unittest
from pathlib import Path

from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import controller_row
import psarc
from psarc_fixtures import archive as fixture

# The shipped layout's Adjust settings rows of each Controls screen, byte for byte (data09.psarc):
# Motion Sensor, Motion Sensitivity, Airbrake Sensitivity and the switched-off Thrust Sensitivity
# slider, 1839 and 1785 bytes.
P1 = (b'<MotionList name="Motion Sensor" global="Motion Sensor" focus="true" default="FE_OFF" '
      b'delay="0.35">\r\n              <Values idstring="OPT_CTRL_MOTION_CTRL" x="0" y="645" '
      b'color="FEGlobals->TextColor" arrowcolor="FEGlobals->TextColor" gap="0" sideWidth="790" '
      b'rightmin="700" rightmax="805"></Values>\r\n              <Entry idstring="FE_OFF" value="motion '
      b'off"></Entry>\r\n              <Entry idstring="OPT_CTRL_PITCH" value="motion pitch"></Entry>\r\n    '
      b'          <Entry idstring="OPT_CTRL_PAS" value="motion all"></Entry>\r\n            </MotionList>\r\n'
      b'\r\n            <Slider name="Motion Sensitivity" global="Motion Sensitivity" focus="true" '
      b'save="true" default="40%" delay="0.4">\r\n              <Values idstring="FE_MOTION_SENSITIVITY" '
      b'font="default" scale="0.8" x="0" y="690" color="FEGlobals->TextColor" '
      b'arrowcolor="FEGlobals->TextColor" gap="0" minSlide="20" maxSlide="100" incSlide="10" txtSlide="%" '
      b'sideWidth="790" rightmin="700" rightmax="805"></Values>\r\n            </Slider>\r\n'
      b'\t\t\t\t\t\t<Slider name="Airbrake Sensitivity" global="Airbrake Left Sensitivity" focus="true" '
      b'save="true" default="100%" delay="0.45">\r\n              <Values idstring="FE_AIRBRAKE_SENSITIVITY" '
      b'font="default" scale="0.8" x="0" y="735" color="FEGlobals->TextColor" '
      b'arrowcolor="FEGlobals->TextColor" gap="0" minSlide="10" maxSlide="100" incSlide="10" txtSlide="%" '
      b'sideWidth="790" rightmin="700" rightmax="805"></Values>\r\n            </Slider>\r\n            '
      b'<aSlider name="Thrust Sensitivity" global="Thrust Sensitivity" focus="true" save="true" '
      b'default="30%" delay="0.5">\r\n              <Values idstring="OPT_CTRL_ACC_SENS" font="default" '
      b'scale="0.8" x="0" y="780" color="FEGlobals->TextColor" arrowcolor="FEGlobals->TextColor" gap="0" '
      b'minSlide="10" maxSlide="100" incSlide="10" txtSlide="%" sideWidth="790" rightmin="700" '
      b'rightmax="805"></Values>\r\n            </aSlider>'
      )
P2 = (b'<MotionList name="Motion Sensor" global="Motion Sensor" focus="true" default="FE_OFF" delay="0.35" '
      b'InputPad="1">\r\n      <Values idstring="OPT_CTRL_MOTION_CTRL" x="0" y="645" '
      b'color="FEGlobals->TextColor" arrowcolor="FEGlobals->TextColor" gap="0" sideWidth="790" '
      b'rightmin="700" rightmax="805"></Values>\r\n      <Entry idstring="FE_OFF" value="motion '
      b'off"></Entry>\r\n      <Entry idstring="OPT_CTRL_PITCH" value="motion pitch"></Entry>\r\n      '
      b'<Entry idstring="OPT_CTRL_PAS" value="motion all"></Entry>\r\n    </MotionList>\r\n\r\n    <Slider '
      b'name="Motion Sensitivity" global="Motion Sensitivity" focus="true" save="true" default="40%" '
      b'delay="0.4" InputPad="1">\r\n      <Values idstring="FE_MOTION_SENSITIVITY" font="default" '
      b'scale="0.8" x="0" y="690" color="FEGlobals->TextColor" arrowcolor="FEGlobals->TextColor" gap="0" '
      b'minSlide="20" maxSlide="100" incSlide="10" txtSlide="%" sideWidth="790" rightmin="700" '
      b'rightmax="805"></Values>\r\n    </Slider>\r\n    <Slider name="Airbrake Sensitivity" '
      b'global="Airbrake Left Sensitivity" focus="true" save="true" default="100%" delay="0.45" '
      b'InputPad="1">\r\n      <Values idstring="FE_AIRBRAKE_SENSITIVITY" font="default" scale="0.8" x="0" '
      b'y="735" color="FEGlobals->TextColor" arrowcolor="FEGlobals->TextColor" gap="0" minSlide="10" '
      b'maxSlide="100" incSlide="10" txtSlide="%" sideWidth="790" rightmin="700" rightmax="805"></Values>\r\n'
      b'    </Slider>\r\n    <aSlider name="Thrust Sensitivity" global="Thrust Sensitivity" focus="true" '
      b'save="true" default="30%" delay="0.5" InputPad="1">\r\n      <Values idstring="OPT_CTRL_ACC_SENS" '
      b'font="default" scale="0.8" x="0" y="780" color="FEGlobals->TextColor" '
      b'arrowcolor="FEGlobals->TextColor" gap="0" minSlide="10" maxSlide="100" incSlide="10" txtSlide="%" '
      b'sideWidth="790" rightmin="700" rightmax="805"></Values>\r\n    </aSlider>'
      )
LAYOUT = (b'<Screen type="Controls" name="Controls">\r\n  ' + P1 + b'\r\n</Screen>\r\n'
          b'<Screen type="Controls" name="ControlsP2">\r\n  ' + P2 + b'\r\n</Screen>\r\n')


def rows(screen):
    """[(name, y, delay)] of a screen's Adjust settings rows, in the layout's order."""
    found = re.findall(rb'<(?:List|MotionList|Slider|aSlider) name="([^"]*)".*? delay="([^"]*)".*? y="([^"]*)"',
                       screen, re.S)
    return [(name.decode(), y.decode(), delay.decode()) for name, delay, y in found]


class RowTests(unittest.TestCase):
    def test_fixture_is_the_shipped_layout(self):
        self.assertEqual((len(P1), len(P2)), (1839, 1785))

    def test_the_row_comes_first_in_the_same_bytes(self):
        new = controller_row.add_row(LAYOUT)
        self.assertEqual(len(new), len(LAYOUT))
        self.assertNotIn(b'aSlider', new)
        first, second = new.split(b'</Screen>\r\n')[:2]
        self.assertEqual(rows(first), [('Pad Style P1', '645', '0.35'), ('Motion Sensor', '690', '0.4'),
                                       ('Motion Sensitivity', '735', '0.45'), ('Airbrake Sensitivity', '780', '0.5')])
        self.assertEqual(rows(second), [('Pad Style P2', '645', '0.35'), ('Motion Sensor', '690', '0.4'),
                                        ('Motion Sensitivity', '735', '0.45'), ('Airbrake Sensitivity', '780', '0.5')])
        self.assertIn(b'<List name="Pad Style P2" global="Pad Style P2" focus="true" save="true" default="auto" '
                      b'delay="0.35" InputPad="1">', new)
        self.assertEqual(second.count(b'InputPad="1"'), 4)
        self.assertEqual(new.count(b'<Entry idstring="AUTO" value="auto"></Entry>\r\n'
                                   b'<Entry idstring="FE_DEFAULT" value="playstation"></Entry>\r\n'
                                   b'<Entry idstring="XBOX" value="xbox"></Entry>'), 2)
        self.assertEqual(new.count(b'idstring="CONTROLLER TYPE"'), 2)
        self.assertTrue(new.startswith(b'<Screen type="Controls" name="Controls">'))

    def test_the_moved_rows_keep_everything_but_their_place(self):
        new = controller_row.add_row(LAYOUT)

        def plain(xml):  # a row's tags without indentation, place or entrance delay
            text = re.sub(rb'\s+', b' ', xml)
            return re.sub(rb' (y|delay)="[^"]*"', b'', text)
        for name in (b'MotionList name="Motion Sensor"', b'Slider name="Motion Sensitivity"',
                     b'Slider name="Airbrake Sensitivity"'):
            tag = name.split()[0]
            old = re.findall(b'<' + name + b'.*?</' + tag + b'>', LAYOUT, re.S)
            self.assertEqual([plain(x) for x in re.findall(b'<' + name + b'.*?</' + tag + b'>', new, re.S)],
                             [plain(x) for x in old])

    def test_a_layout_without_the_sliders_is_unchanged(self):
        xml = b'<Screen name="Controls"></Screen>'
        self.assertEqual(controller_row.add_row(xml), xml)

    def test_a_layout_with_only_one_screen_is_refused(self):
        with self.assertRaises(ValueError):
            controller_row.add_row(P1)

    def test_a_slider_without_the_rows_above_it_is_refused(self):
        slider = P1[P1.index(b'<aSlider'):]
        with self.assertRaises(ValueError):
            controller_row.add_row(slider + slider.replace(b'delay="0.5">', b'delay="0.5" InputPad="1">'))

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
