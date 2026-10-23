from __future__ import annotations

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "third_party/mc2-loader/source/reimpl"


class StreamingCacheTests(unittest.TestCase):
    def test_real_file_reads_and_concurrent_fills(self) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")
        source = (SOURCE / "io.c").read_text()
        begin = source.index("#define TEXTURE_CACHE_SLOTS")
        end = source.index("static VirtualObbStream *virtual_obb_from_file", begin)
        with tempfile.TemporaryDirectory() as directory:
            scratch = Path(directory)
            (scratch / "cache_under_test.h").write_text(source[begin:end])
            executable = scratch / "streaming-cache-test"
            subprocess.run(
                [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                 "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-fsanitize=undefined", "-fno-sanitize-recover=all",
                 "-I", directory, str(SOURCE / "io_cache_test.c"),
                 "-o", str(executable)],
                check=True, timeout=30,
            )
            subprocess.run([str(executable), str(SOURCE / "testdata/car_cops.dds-header")],
                           check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
