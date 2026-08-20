#!/usr/bin/env bash
# run_tests.sh — every host unit test that covers code compiled into the SHIPPED
# firmware. These run on a Mac/Linux box in about a second: the point is that the
# tricky pure logic (sound-command decoding, the mixer, WAV parsing) is proven
# without powering up a pinball to ask it a question.
#
#   ./tools/run_tests.sh          run everything, non-zero exit on any failure
#
# NOT run here (on purpose): the PSOROM / chef benches (tools/test_chef*,
# host_chefsim, host_psorom_test, chef_fuzz, pair_scan, keeps_scan...). They
# exercise the 6502 + YM2151 emulation path, which is EXCLUDED from the S3
# firmware (see build_src_filter in platformio.ini), and most of them need real
# Gottlieb sound ROMs that are not in this repo.
#
# (C) 2026 Valere Pillet / Pstore.
set -uo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-g++}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0
ran=0

run() {                      # run <name> <sources...>
  local name=$1; shift
  printf '%-14s ' "$name"
  if ! $CXX -std=c++17 -O1 -Isrc -o "$TMP/$name" "$@" 2> "$TMP/$name.cc.log"; then
    echo "BUILD FAILED"; sed 's/^/    /' "$TMP/$name.cc.log" | head -20; fails=$((fails+1)); return
  fi
  if "$TMP/$name" > "$TMP/$name.log" 2>&1; then
    echo "PASS  ($(grep -ciE '^[[:space:]]*(OK|PASS)' "$TMP/$name.log") checks)"
  else
    echo "FAIL"; sed 's/^/    /' "$TMP/$name.log"; fails=$((fails+1))
  fi
  ran=$((ran+1))
}

echo "== host unit tests (code that also runs on the board)"
# sndmap: the per-title sound-command decoder — banks, headers, remaps, release
# tokens, de-bounce, and every malformed-input case.
run sndmap  tools/test_sndmap.cpp src/sndmap.cpp
# wavmix: voice allocation, looping, starvation recovery, the soft-knee limiter.
run wavmix  tools/test_wavmix.cpp src/wavmix.cpp
# the whole WAV chain: file parsing, resampling source, set/attribute handling.
run wavchain tools/host_wav_test.cpp src/wavmix.cpp src/wavfile.cpp src/wavsrc.cpp src/wavset.cpp

# romcrypt needs mbedTLS headers, which the ESP toolchain has and a bare Mac does
# not. Skipped loudly rather than silently: `brew install mbedtls` enables it.
printf '%-14s ' "romcrypt"
if $CXX -std=c++17 -O1 tools/host_romcrypt_test.cpp -lmbedcrypto -o "$TMP/rct" 2>/dev/null; then
  if "$TMP/rct" > "$TMP/rct.log" 2>&1; then echo "PASS"; ran=$((ran+1));
  else echo "FAIL"; sed 's/^/    /' "$TMP/rct.log"; fails=$((fails+1)); fi
else
  echo "SKIP  (mbedTLS not installed on this host: brew install mbedtls)"
fi

echo
if [ "$fails" -eq 0 ]; then echo "== all $ran suites passed"; else echo "== $fails of $((ran+fails)) suites FAILED"; fi
exit $fails
