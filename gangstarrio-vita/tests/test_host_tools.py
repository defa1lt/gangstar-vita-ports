from __future__ import annotations

import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import verify_game_files  # noqa: E402
import prepare_data  # noqa: E402
import audit_loader_baseline  # noqa: E402


def synthetic_arm_elf() -> bytes:
    header = bytearray(52)
    header[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHI", header, 16, 3, 40, 1)
    struct.pack_into("<I", header, 36, 0x05000000)
    return bytes(header)


class ElfVerificationTests(unittest.TestCase):
    def test_parses_supported_identity(self) -> None:
        identity = verify_game_files.parse_elf_identity(synthetic_arm_elf())
        self.assertEqual(identity.elf_class, 1)
        self.assertEqual(identity.byte_order, 1)
        self.assertEqual(identity.elf_type, 3)
        self.assertEqual(identity.machine, 40)
        self.assertEqual(identity.eabi_version, 5)

    def test_rejects_non_elf(self) -> None:
        with self.assertRaisesRegex(verify_game_files.VerificationError, "ELF32"):
            verify_game_files.parse_elf_identity(b"not an ELF")

    def test_accepts_matching_profile(self) -> None:
        data = synthetic_arm_elf()
        digest = hashlib.sha256(data).hexdigest()
        with (
            mock.patch.object(verify_game_files, "SO_SIZE", len(data)),
            mock.patch.object(verify_game_files, "SO_SHA256", digest),
        ):
            report = verify_game_files.verify_bytes(data, "fixture")
        self.assertTrue(report.supported)

    def test_rejects_wrong_hash(self) -> None:
        data = synthetic_arm_elf()
        with mock.patch.object(verify_game_files, "SO_SIZE", len(data)):
            with self.assertRaisesRegex(
                verify_game_files.VerificationError, "SHA-256"
            ):
                verify_game_files.verify_bytes(data, "fixture")


class Sha256ImplementationTests(unittest.TestCase):
    def test_c_implementation_vectors(self) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "sha256-test"
            subprocess.run(
                [
                    compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "include"),
                    str(ROOT / "source" / "sha256.c"),
                    str(ROOT / "tests" / "sha256_test.c"),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


class GameloftObbTests(unittest.TestCase):
    def test_accepts_gameloft_container_header(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "archive.obb"
            entry_count = 2
            names_offset = 16 + entry_count * 16
            data_offset = names_offset + 12
            path.write_bytes(
                struct.pack(">IIII", 0, data_offset, names_offset, entry_count)
                + bytes(data_offset - 16)
            )
            report = prepare_data.inspect_gameloft_obb(path)
        self.assertEqual(report, {"entries": 2, "data_offset": data_offset})

    def test_rejects_wrong_names_offset(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "archive.obb"
            path.write_bytes(struct.pack(">IIII", 0, 64, 17, 2) + bytes(48))
            with self.assertRaisesRegex(
                verify_game_files.VerificationError, "names offset"
            ):
                prepare_data.inspect_gameloft_obb(path)


class LoaderBaselineTests(unittest.TestCase):
    def test_extracts_undefined_symbols(self) -> None:
        output = """
Symbol table '.dynsym' contains 3 entries:
   Num:    Value  Size Type    Bind   Vis      Ndx Name
     1: 00000000     0 FUNC    GLOBAL DEFAULT  UND malloc
     2: 00100000    12 FUNC    GLOBAL DEFAULT    7 local_symbol
"""
        self.assertEqual(
            audit_loader_baseline.parse_undefined_symbols(output), {"malloc"}
        )

    def test_extracts_literal_dynlib_entries(self) -> None:
        source = '{ "malloc", (uintptr_t)&malloc },\n{ "free", 0 },\n'
        self.assertEqual(
            audit_loader_baseline.parse_mapping_names(source), {"malloc", "free"}
        )

    def test_rio_extension_defines_all_missing_names(self) -> None:
        source = (ROOT / "source" / "rio_missing_imports.c").read_text()
        self.assertEqual(
            audit_loader_baseline.parse_mapping_names(source),
            {
                "__cxa_deleted_virtual", "__cxa_pure_virtual",
                "_ZSt9terminatev",
                "closelog", "getegid", "geteuid", "getgid", "getpwuid",
                "getuid", "openlog", "pread", "statfs", "syslog",
                "zError", "zlibVersion",
            },
        )


if __name__ == "__main__":
    unittest.main()
