"""PlatformIO pre-build hook: regenerate include/super_flashy_levels.h from
levels/*.txt so a new or edited level is picked up by a normal build/upload."""

import sys
from pathlib import Path

Import("env")  # noqa: F821  (provided by PlatformIO)

sys.path.insert(0, str(Path(env["PROJECT_DIR"]) / "tools"))  # noqa: F821
import build_levels  # noqa: E402

if build_levels.main(["--quiet"]) != 0:
    env.Exit(1)  # noqa: F821
