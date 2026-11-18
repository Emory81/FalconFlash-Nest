import os
import shutil
import subprocess
from pathlib import Path


Import("env")


def load_dotenv(path):
    values = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip("\"'")
    return values


project_dir = Path(env.subst("$PROJECT_DIR"))
dotenv = load_dotenv(project_dir / ".env")
required = ("FALCON_FACTORY_ID", "FALCON_FACTORY_SECRET")
missing = [key for key in required if not dotenv.get(key)]
if missing:
    raise RuntimeError(
        "Missing {} in .env".format(", ".join(missing))
    )

game_mode_value = dotenv.get("GAME_MODE_ONLY", "false").lower()
game_mode_only = "1" if game_mode_value in ("1", "true", "yes", "on") else "0"
api_base_url = dotenv.get(
    "FALCON_API_BASE_URL",
    "https://falconflash.emory.work/api",
).rstrip("/")
game_asset_base_url = dotenv.get(
    "FALCON_GAME_ASSET_BASE_URL",
    "https://raw.githubusercontent.com/Emory81/FalconFlash-Nest/main/game_assets",
)


def normalize_version(version):
    version = version.strip()
    if version.lower().startswith("v"):
        version = version[1:]
    return version or "0.0.0-dev"


def run_git(*arguments):
    try:
        result = subprocess.run(
            ["git", *arguments],
            cwd=str(project_dir),
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError:
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def firmware_version():
    version_override = os.environ.get("FALCON_FIRMWARE_VERSION_OVERRIDE", "")
    if version_override:
        return normalize_version(version_override)

    github_ref = os.environ.get("GITHUB_REF", "")
    if github_ref.startswith("refs/tags/"):
        return normalize_version(github_ref[len("refs/tags/"):])

    tagged_version = run_git("describe", "--tags", "--exact-match")
    if tagged_version:
        return normalize_version(tagged_version)

    return "0.0.0-dev"


def c_string(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return '\\"' + escaped + '\\"'


build_version = firmware_version()


def preserve_release_firmware(source, target, env):
    if build_version == "0.0.0-dev":
        return
    source_path = Path(str(target[0]))
    release_dir = project_dir / "release_assets" / build_version
    release_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source_path, release_dir / "firmware.bin")


env.Append(
    CPPDEFINES=[
        ("FALCON_FACTORY_ID", c_string(dotenv["FALCON_FACTORY_ID"])),
        ("FALCON_FACTORY_SECRET", c_string(dotenv["FALCON_FACTORY_SECRET"])),
        ("FALCON_API_BASE_URL", c_string(api_base_url)),
        ("FALCON_FIRMWARE_VERSION", c_string(build_version)),
        ("FALCON_GITHUB_REPO", c_string(dotenv.get("FALCON_GITHUB_REPO", ""))),
        ("FALCON_FIRMWARE_ASSET",
         c_string(dotenv.get("FALCON_FIRMWARE_ASSET", "firmware.bin"))),
        ("FALCON_GAME_MODE_ONLY", game_mode_only),
        ("FALCON_GAME_ASSET_BASE_URL", c_string(game_asset_base_url)),
    ]
)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", preserve_release_firmware)
