"""Small manifest tests for the lipo archive selection rules."""

import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("lipo.py")
SPEC = importlib.util.spec_from_file_location("ejit_lipo", SCRIPT)
LIPO = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LIPO)


class ExistingLibsTest(unittest.TestCase):
    def test_sve_off_missing_vectorize_archive_is_optional(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp)
            lib = build / "lib"
            lib.mkdir()
            # Simulate a minimal SVE-OFF build: Vectorize is absent while the
            # core archive needed by the reference link is present.
            (lib / "libLLVMCore.a").touch()

            selected = LIPO.existing_libs(str(build), "x86")

            self.assertIn("libLLVMCore.a", selected)
            self.assertNotIn("libLLVMVectorize.a", selected)

    def test_existing_lib_paths_preserve_spaces(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp) / "build with spaces"
            lib = build / "lib"
            lib.mkdir(parents=True)
            archive = lib / "libLLVMCore.a"
            archive.touch()

            paths = LIPO.existing_lib_paths(str(build), "x86")

            self.assertEqual([str(archive)], paths)


if __name__ == "__main__":
    unittest.main()
