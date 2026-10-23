from __future__ import annotations

import shutil
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "third_party" / "vitaGL" / "source" / "utils" / "tests" / "index_range_test.c"
INCLUDE = ROOT / "third_party" / "vitaGL" / "source" / "utils"


class IndexedRangeHelperTests(unittest.TestCase):
    def _compile_and_run(self, sanitizer: bool, fixture: Path = FIXTURE) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "index-range-test"
            command = [
                compiler,
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(INCLUDE),
                str(fixture),
                "-o",
                str(executable),
            ]
            if sanitizer:
                sanitizer_flags = ["-fsanitize=address,undefined"]
                if sys.platform == "darwin":
                    # The system ASan runtime can hang before main on this
                    # host; UBSan still validates the fixture there.
                    sanitizer_flags = ["-fsanitize=undefined"]
                command[1:1] = sanitizer_flags + [
                    "-fno-omit-frame-pointer",
                    "-fno-sanitize-recover=all",
                ]

            if fixture.name == "multidraw_copy_test.c":
                source = (INCLUDE.parent / "custom_shaders.c").read_text()
                macro_start = source.index("#define handle_unpacked_attrib(first, count)",
                                           source.index("#else", source.index("#ifdef SAFER_DRAW_SPEEDHACK")))
                macro_end = source.index("\n#endif", macro_start)
                body = source[source.index("void _glMultiDrawArrays_CustomShadersIMPL("):
                              source.index("GLboolean _glDrawArrays_CustomShadersIMPL(")]
                calls = re.findall(r"handle_unpacked_attrib\([^;]+\);", body)
                self.assertEqual(len(calls), 2)
                header = source[macro_start:macro_end] + "\n"
                for index, call in enumerate(calls):
                    call = call.rstrip(";")
                    header += f"#define TEST_MULTIDRAW_COPY_{index}() {call}\n"
                (Path(directory) / "multidraw_copy_under_test.h").write_text(header)
                command[1:1] = ["-I", directory]

            result = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            if result.returncode != 0:
                if sanitizer and (
                    "unsupported option" in result.stderr
                    or "unknown argument" in result.stderr
                    or "unrecognized command-line option" in result.stderr
                ):
                    self.skipTest("compiler does not support ASan/UBSan")
                self.fail(
                    "C fixture compilation failed:\n"
                    + result.stdout
                    + result.stderr
                )

            try:
                run = subprocess.run(
                    [str(executable)],
                    check=False,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
            except subprocess.TimeoutExpired:
                if sanitizer:
                    self.skipTest("ASan/UBSan runtime is unavailable on this host")
                raise
            self.assertEqual(
                run.returncode,
                0,
                msg=run.stdout + run.stderr,
            )

    def test_index_range_helpers(self) -> None:
        self._compile_and_run(sanitizer=False)

    def test_index_range_helpers_under_sanitizers(self) -> None:
        self._compile_and_run(sanitizer=True)

    def test_vertex_layout(self) -> None:
        self._compile_and_run(False, INCLUDE / "tests" / "vertex_layout_test.c")

    def test_vertex_layout_under_sanitizers(self) -> None:
        self._compile_and_run(True, INCLUDE / "tests" / "vertex_layout_test.c")

    def test_multidraw_copy_with_guard_page(self) -> None:
        self._compile_and_run(False, INCLUDE / "tests" / "multidraw_copy_test.c")

    def test_multidraw_copy_with_guard_page_under_sanitizers(self) -> None:
        self._compile_and_run(True, INCLUDE / "tests" / "multidraw_copy_test.c")

    def test_available_texture_mips(self) -> None:
        self._compile_and_run(False, ROOT / "third_party" / "mc2-loader" / "source" / "utils" / "texture_mip_test.c")

    def test_available_texture_mips_under_sanitizers(self) -> None:
        self._compile_and_run(True, ROOT / "third_party" / "mc2-loader" / "source" / "utils" / "texture_mip_test.c")


if __name__ == "__main__":
    unittest.main()
