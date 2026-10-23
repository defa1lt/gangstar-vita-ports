from __future__ import annotations

import ctypes
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ControllerTutorialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("host C compiler unavailable")
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        wrapper = Path(cls.directory.name) / "labels.c"
        wrapper.write_text('#include "controller_labels.h"\n'
                           'int translate(char *o, size_t n, const char *s, unsigned i, int r) '
                           '{ return controller_tutorial_text(o, n, s, i, r); }\n')
        library = Path(cls.directory.name) / "labels.so"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-fPIC", "-I", str(ROOT / "include"),
                        str(wrapper), "-o", str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.function = cls.library.translate
        cls.function.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p,
                                 ctypes.c_uint, ctypes.c_int]
        cls.function.restype = ctypes.c_int

    def translate(self, text, index=625, russian=False, capacity=2048):
        output = ctypes.create_string_buffer(max(capacity, 1))
        result = self.function(output, capacity, text.encode(), index, russian)
        return result, output.value.decode()

    def test_face_names_keep_formatting(self):
        text = r'Use \0A button\1, B button, X button and Y button.'
        self.assertEqual(self.translate(text),
                         (1, r'Use \0cross button\1, circle button, square button and triangle button.'))
        self.assertEqual(self.translate('<font>A button</font>'),
                         (1, '<font>cross button</font>'))

    def test_russian_and_cyrillic_a(self):
        text = 'кнопку A / кнопку А / кнопку B / кнопку X / кнопку Y'
        self.assertEqual(self.translate(text, russian=True),
                         (1, 'крестик / крестик / круг / квадрат / треугольник'))

    def test_road_and_flight_have_distinct_controls(self):
        text = 'A button / B button / right trigger / left trigger'
        self.assertEqual(self.translate(text, index=628), (1, 'R / L / R / L'))
        self.assertEqual(self.translate(text, index=629),
                         (1, 'cross button / circle button / R / L'))
        self.assertEqual(self.translate('кнопку А и кнопку B', index=648, russian=True),
                         (1, 'R и L'))
        self.assertEqual(self.translate('right and left triggers', index=635), (1, 'R / L'))

    def test_unrelated_strings_and_ids_are_unchanged(self):
        for index in [0, 624, 653, 9999]:
            self.assertEqual(self.translate('A button / B button', index=index),
                             (0, 'A button / B button'))
        self.assertEqual(self.translate('Walk towards the marker.'),
                         (0, 'Walk towards the marker.'))

    def test_capacity_including_terminator(self):
        self.assertEqual(self.translate('A button', capacity=13), (1, 'cross button'))
        self.assertEqual(self.translate('A button', capacity=12), (-1, ''))
        self.assertEqual(self.translate('A button', capacity=0), (-1, ''))
        self.assertEqual(self.translate('', capacity=1), (0, ''))

    def test_available_native_localization_tables(self):
        resource = ROOT / 'reference/android/extracted_obb/main.12030.com.gameloft.android.ANMP.GloftG4HM/_packRes'
        if not (resource / 'global.russian').exists():
            self.skipTest('user-owned localization tables unavailable')
        for language in ['english', 'russian']:
            data = (resource / ('global.' + language)).read_bytes()
            count = struct.unpack_from('<H', data)[0]
            strings, position = [], 2
            for _ in range(count):
                length = struct.unpack_from('<H', data, position)[0]
                position += 2
                strings.append(data[position:position + length].decode())
                position += length
            self.assertEqual(position, len(data))
            self.assertEqual(count, 653)
            updated = set()
            for index, original in enumerate(strings):
                result, text = self.translate(original, index=index,
                                              russian=language == 'russian', capacity=4096)
                self.assertGreaterEqual(result, 0)
                if not 625 <= index <= 652:
                    self.assertEqual((result, text), (0, original))
                    continue
                if result:
                    updated.add(index)
                self.assertIsNone(re.search(r'[ABXY] button|кнопку [ABXYА]', text))
                for marker in [r'\0', r'\1']:
                    self.assertEqual(original.count(marker), text.count(marker))
            self.assertEqual(updated, {625, 626, 627, 628, 629, 630, 632, 633,
                                       634, 635, 644, 645, 646, 647, 648, 650, 651})


if __name__ == '__main__':
    unittest.main()
