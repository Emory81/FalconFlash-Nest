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


def c_string(value):
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return '\\"' + escaped + '\\"'


env.Append(
    CPPDEFINES=[
        ("FALCON_FACTORY_ID", c_string(dotenv["FALCON_FACTORY_ID"])),
        ("FALCON_FACTORY_SECRET", c_string(dotenv["FALCON_FACTORY_SECRET"])),
        ("FALCON_GAME_MODE_ONLY", game_mode_only),
    ]
)
