#!/usr/bin/env bash
# Flash the arena-matter firmware to an ESP32-S3 DevKitC-1 over USB.
#
# Why this exists: OTA does not work on the S3 build. The board crashes about
# two seconds into a 1.6 MB upload (measured 2026-08-02: it goes silent after
# ~250 kB and reboots into the OLD image, so the update looks like it succeeded
# — uptime resets — while nothing was written). Until that is fixed, USB is the
# only way to change the S3 firmware. On the WROOM/D1 Mini, OTA is fine.
#
#   ./tools/s3_flash_usb.sh [dossier_des_bin] [port]
#
# The bin directory needs bootloader.bin, partition-table.bin,
# ota_data_initial.bin and arena_matter.bin, all from the SAME build.
# The spiffs partition and NVS are deliberately left alone: the web UI, the
# game bundle and the LED mapping survive the flash.
set -euo pipefail

BINDIR="${1:-}"
PORT="${2:-}"
ESPTOOL_PY="${ESPTOOL:-}"

die() { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }

[ -n "$BINDIR" ] || die "usage: $0 <dossier_des_bin> [port]"
for f in bootloader.bin partition-table.bin ota_data_initial.bin arena_matter.bin; do
  [ -f "$BINDIR/$f" ] || die "manquant: $BINDIR/$f"
done

# --- port ------------------------------------------------------------------
# macOS lists cu.debug-console and cu.Bluetooth-Incoming-Port as serial ports;
# neither is a board. Only USB bridges and the S3's own native USB count.
if [ -z "$PORT" ]; then
  mapfile -t PORTS < <(ls /dev/cu.usbserial* /dev/cu.usbmodem* /dev/cu.wchusbserial* \
                          /dev/cu.SLAB_USBtoUART* 2>/dev/null || true)
  [ "${#PORTS[@]}" -gt 0 ] || die "aucun port USB. La carte est-elle branchee ?"
  if [ "${#PORTS[@]}" -gt 1 ]; then
    echo "Plusieurs ports :"; printf '  %s\n' "${PORTS[@]}"
    die "precise lequel : $0 $BINDIR /dev/cu.xxxx"
  fi
  PORT="${PORTS[0]}"
fi
echo "port : $PORT"

# --- esptool ---------------------------------------------------------------
if [ -z "$ESPTOOL_PY" ]; then
  for c in "$(dirname "$0")/../.venv/bin/python" \
           /private/tmp/claude-501/-Users-vapi27/*/scratchpad/.venv/bin/python; do
    [ -x "$c" ] && { ESPTOOL_PY="$c"; break; }
  done
fi
[ -n "$ESPTOOL_PY" ] || die "esptool introuvable. python3 -m venv .venv && .venv/bin/pip install esptool"

# --- flash -----------------------------------------------------------------
# 0x0 for the S3 bootloader (the original ESP32 puts it at 0x1000).
# ota_data_initial resets the boot slot to ota_0, which is where the app goes.
exec "$ESPTOOL_PY" -m esptool --chip esp32s3 -b 460800 \
  --port "$PORT" --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0      "$BINDIR/bootloader.bin" \
  0xc000   "$BINDIR/partition-table.bin" \
  0x15000  "$BINDIR/ota_data_initial.bin" \
  0x20000  "$BINDIR/arena_matter.bin"
