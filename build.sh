#!/usr/bin/env bash
# build.sh — produce the shippable ESP firmware set, reproducibly.
#
#   ./build.sh            build the v1 target (esp32s3) -> dist/
#   ./build.sh --all      also compile-check the experimental esp32c3 target
#   ./build.sh --clean    wipe .pio/build first (use before tagging a release)
#
# Outputs, all in dist/:
#   firmware.bin      the application (OTA-able: POST it to /ota)
#   littlefs.bin      the web UI filesystem image (data/ -> the spiffs partition)
#   bootloader.bin    second-stage bootloader   } needed only for a FIRST,
#   partitions.bin    partition table           } cable-attached flash
#   MANIFEST.txt      sha256 + sizes + git commit + toolchain + flash offsets
#
# REPRODUCIBILITY — measured on 2026-07-30, not assumed:
#   firmware.bin  IS byte-identical when the same commit is rebuilt from the same
#                 absolute path (`./build.sh --clean` twice -> same sha256). That
#                 works because version.py stamps the COMMIT DATE, never the wall
#                 clock. It is NOT identical from a different directory: the
#                 toolchain bakes absolute source paths into the image.
#   littlefs.bin  is NOT reproducible at all — mklittlefs records each file's
#                 mtime, which changes on every checkout or copy. Its sha256 in
#                 MANIFEST.txt identifies the artifact you shipped; it is not a
#                 value someone else can reproduce.
# Build from a CLEAN tree — a dirty tree is stamped "-dirty" and MANIFEST.txt
# says so, loudly.
#
# (C) 2026 Valere Pilpil / Pstore.
set -euo pipefail

cd "$(dirname "$0")"
ENV_MAIN=esp32s3
BUILD=.pio/build/$ENV_MAIN
DIST=dist
ALL=0

for a in "$@"; do
  case "$a" in
    --all)   ALL=1 ;;
    --clean) echo "==> cleaning"; rm -rf .pio/build ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown option: $a (try --help)" >&2; exit 2 ;;
  esac
done

command -v pio >/dev/null || { echo "PlatformIO (pio) not found in PATH" >&2; exit 1; }

# A file marked skip-worktree / assume-unchanged is edited locally and silently
# NEVER committed. That is how include/board_config.h drifted until a fresh clone
# of main stopped compiling (missing PIN_RGB_LED) while this laptop built fine.
# Never again: refuse to produce a release from a tree that hides files.
HIDDEN=$(git ls-files -v 2>/dev/null | grep -v '^H ' || true)
if [ -n "$HIDDEN" ]; then
  echo "ERROR: these tracked files are hidden from git (skip-worktree/assume-unchanged);" >&2
  echo "       your local edits to them will NOT be in the release:" >&2
  echo "$HIDDEN" | sed 's/^/       /' >&2
  echo "  fix: git update-index --no-skip-worktree <file>" >&2
  exit 1
fi

GIT_SHA=$(git rev-parse HEAD 2>/dev/null || echo "no-git")
GIT_SHORT=$(git rev-parse --short=7 HEAD 2>/dev/null || echo "nogit")
GIT_DATE=$(git log -1 --format=%cd --date=short 2>/dev/null || echo unknown)
GIT_TAG=$(git describe --tags --exact-match 2>/dev/null || echo "(untagged)")
if [ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]; then
  DIRTY="  *** DIRTY WORKING TREE — NOT A REPRODUCIBLE RELEASE BUILD ***"
  GIT_SHORT="$GIT_SHORT-dirty"
else
  DIRTY=""
fi
FW_VERSION=$(sed -n 's/^#define FW_VERSION[[:space:]]*"\(.*\)".*/\1/p' include/board_config.h)

echo "==> GottFA80-PLuS ESP  v$FW_VERSION+$GIT_SHORT  ($GIT_DATE)$DIRTY"

echo "==> host unit tests"
./tools/run_tests.sh

echo "==> firmware  ($ENV_MAIN)"
pio run -e $ENV_MAIN
echo "==> filesystem image  (data/ -> littlefs)"
pio run -e $ENV_MAIN -t buildfs

if [ "$ALL" = 1 ]; then
  echo "==> compile-check the EXPERIMENTAL esp32c3 target (not shipped — see platformio.ini)"
  pio run -e esp32c3
fi

rm -rf "$DIST"; mkdir -p "$DIST"
for f in firmware.bin littlefs.bin bootloader.bin partitions.bin; do
  cp "$BUILD/$f" "$DIST/$f"
done
cp "$BUILD/firmware.elf" "$DIST/firmware.elf"     # keep it: the only way to decode a panic backtrace

# sha256sum is coreutils; macOS ships shasum. Support both.
if command -v sha256sum >/dev/null; then SHA="sha256sum"; else SHA="shasum -a 256"; fi

{
  echo "GottFA80-PLuS — ESP32-S3 companion firmware"
  echo "==========================================="
  echo
  echo "version     : $FW_VERSION"
  echo "build id    : $FW_VERSION+$GIT_SHORT"
  echo "git commit  : $GIT_SHA"
  echo "git tag     : $GIT_TAG"
  echo "commit date : $GIT_DATE"
  echo "built by    : $(uname -srm)"
  echo "platformio  : $(pio --version 2>/dev/null | tr -d '\n')"
  [ -n "$DIRTY" ] && { echo; echo "!!! $DIRTY"; echo "!!! Uncommitted changes were compiled in. Do not ship this."; }
  echo
  echo "artifacts (sha256  size  name)"
  echo "------------------------------"
  for f in firmware.bin littlefs.bin bootloader.bin partitions.bin; do
    printf '%s  %9d  %s\n' "$($SHA "$DIST/$f" | cut -d' ' -f1)" "$(wc -c < "$DIST/$f")" "$f"
  done
  echo
  echo "flash offsets (ESP32-S3, board_build.partitions = default_16MB.csv)"
  echo "-------------------------------------------------------------------"
  echo "  0x000000  bootloader.bin"
  echo "  0x008000  partitions.bin"
  echo "  0x010000  firmware.bin      (app0; OTA writes the other slot, app1 @0x650000)"
  echo "  0xc90000  littlefs.bin      (web UI)"
  echo
  echo "first flash, board on USB:"
  echo "  pio run -e esp32s3 -t upload && pio run -e esp32s3 -t uploadfs"
  echo "update over WiFi, no cable:"
  echo "  curl -F 'file=@dist/firmware.bin' http://<board>/ota"
  echo "  curl -F 'file=@data/index.html'   http://<board>/fsup    # web UI only"
  echo
  echo "verify the board actually runs this build:"
  echo "  curl -s http://<board>/sysinfo    -> \"fw\":\"$FW_VERSION\",\"git\":\"$GIT_SHORT\""
  echo
  echo "reproducibility (measured, not assumed)"
  echo "---------------------------------------"
  echo "  firmware.bin  same commit + same absolute build path -> identical sha256."
  echo "                A different directory gives a different hash: the toolchain"
  echo "                bakes absolute source paths into the image."
  echo "  littlefs.bin  NOT reproducible - mklittlefs stores per-file mtimes, which"
  echo "                change on every checkout. The hash above identifies the file"
  echo "                that was shipped; it is not one a third party can recompute."
  echo
  echo "NOT included in this release (deliberately — see README.md):"
  echo "  - esp32c3: compiles, never run on hardware, no sound/display/time-attack"
  echo "  - gosowav*: PSOROM bench environments, not product firmware"
} > "$DIST/MANIFEST.txt"

echo
cat "$DIST/MANIFEST.txt"
echo "==> done -> $DIST/"
