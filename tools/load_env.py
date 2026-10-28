import os
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


env.Append(
    CPPDEFINES=[
        ("FALCON_FACTORY_ID", c_string(dotenv["FALCON_FACTORY_ID"])),
        ("FALCON_FACTORY_SECRET", c_string(dotenv["FALCON_FACTORY_SECRET"])),
        ("FALCON_FIRMWARE_VERSION", c_string(firmware_version())),
        ("FALCON_GITHUB_REPO", c_string(dotenv.get("FALCON_GITHUB_REPO", ""))),
        ("FALCON_FIRMWARE_ASSET",
         c_string(dotenv.get("FALCON_FIRMWARE_ASSET", "firmware.bin"))),
        ("FALCON_GAME_MODE_ONLY", game_mode_only),
    ]
)
