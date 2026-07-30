# version.py — PlatformIO pre-script: stamp the build with its git identity.
#
# Injects two preprocessor defines the firmware reports on /sysinfo, /link and in
# the web UI, so a board recovered from a customer's basement can be identified
# exactly:
#
#   FW_GIT    "951b327"  or "951b327-dirty"  or "nogit" (built outside a checkout)
#   FW_BUILD  "2026-07-30"                   the COMMIT date, not the wall clock
#
# WHY the commit date and not `datetime.now()`: a wall-clock stamp makes every
# rebuild of the same source produce a different binary, so the sha256 in
# MANIFEST.txt could never be reproduced by anyone else. Deriving the stamp from
# the commit keeps `build.sh` byte-for-byte repeatable from a clean checkout.
#
# (C) 2026 Valere Pilpil / Pstore.
import subprocess

Import("env")  # noqa: F821  (injected by SCons)


def _git(*args):
    try:
        out = subprocess.check_output(
            ["git"] + list(args),
            cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
            stderr=subprocess.DEVNULL,
        )
        return out.decode("utf-8", "replace").strip()
    except Exception:
        return ""


sha = _git("rev-parse", "--short=7", "HEAD")
if not sha:
    sha, stamp = "nogit", "unknown"
else:
    if _git("status", "--porcelain", "--untracked-files=no"):
        sha += "-dirty"
    stamp = _git("log", "-1", "--format=%cd", "--date=short") or "unknown"

env.Append(  # noqa: F821
    CPPDEFINES=[
        ("FW_GIT", env.StringifyMacro(sha)),      # noqa: F821
        ("FW_BUILD", env.StringifyMacro(stamp)),  # noqa: F821
    ]
)
print("[version] FW_GIT=%s FW_BUILD=%s" % (sha, stamp))
