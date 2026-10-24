Import("env")

import json
import re
import sys
from pathlib import Path

manifest = Path(env["PROJECT_DIR"]) / "version.json"
try:
    data = json.loads(manifest.read_text(encoding="utf-8"))
    version = str(data["version"]).strip()
    firmware = str(data["firmware"]).strip()
except Exception as exc:
    sys.stderr.write("version.json needs version and firmware: %s\n" % exc)
    env.Exit(1)

if version[:1] in ("v", "V"):
    version = version[1:]

if re.fullmatch(r"\d+(?:\.\d+){1,3}", version) is None:
    sys.stderr.write("version.json version must look like 1.2.3, got %s\n" % version)
    env.Exit(1)

expected = "/v%s/firmware.bin" % version
if not firmware.startswith("https://") or expected not in firmware:
    sys.stderr.write("version.json firmware URL must be https and contain %s\n" % expected)
    env.Exit(1)

sys.stderr.write("Firmware version from version.json: %s\n" % version)
sys.stderr.write("Firmware URL from version.json: %s\n" % firmware)
env.Append(CPPDEFINES=[("CURRENT_VERSION", '\\"%s\\"' % version)])
