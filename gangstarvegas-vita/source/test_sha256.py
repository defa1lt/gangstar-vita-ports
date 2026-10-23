"""Cross-check the actual C SHA-256 used by the Vita loader against hashlib."""
from pathlib import Path
import hashlib
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Sha256Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = ROOT / ".agents/scratch"
        scratch.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(dir=scratch)
        cls.binary = Path(cls.temp.name) / "sha256-stream"
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                        str(ROOT / "source/sha256.c"), str(ROOT / "source/testdata/sha256_stream.c"),
                        "-o", str(cls.binary)], check=True)
    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()
    def test_padding_and_incremental_blocks(self):
        for data in [b"", b"abc"] + [bytes(i % 251 for i in range(n)) for n in (55, 56, 63, 64, 65, 4097)]:
            with self.subTest(length=len(data)):
                actual = subprocess.check_output([str(self.binary)], input=data).decode().strip()
                self.assertEqual(actual, hashlib.sha256(data).hexdigest())
    def test_real_game_library(self):
        path = ROOT / "refs/android/apk/lib/armeabi-v7a/libGangstar4.so"
        if not path.is_file():
            self.skipTest("local original APK reference is not prepared")
        with path.open("rb") as source:
            actual = subprocess.check_output([str(self.binary)], stdin=source).decode().strip()
        with path.open("rb") as source:
            self.assertEqual(actual, hashlib.file_digest(source, "sha256").hexdigest())

if __name__ == "__main__":
    unittest.main()
