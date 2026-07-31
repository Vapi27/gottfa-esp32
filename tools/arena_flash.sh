#!/usr/bin/env bash
# ============================================================================
#  Arena Wall-Art LED — one-command flash + monitor.
#
#  Run this on the machine the ESP32 is plugged into:
#      tools/arena_flash.sh                 # auto-detect the port
#      tools/arena_flash.sh /dev/ttyUSB0    # or name it
#
#  Env overrides:
#      ARENA_ENV=arenaled        target another board (arenaled = S3, arenaled_c3)
#      ARENA_NO_MONITOR=1        flash and exit instead of opening the serial monitor
#      ARENA_SPEED=115200        slower upload if a CH340 keeps dropping the link
#
#  It builds, uploads the web UI to LittleFS, uploads the firmware, then opens
#  the serial monitor. Every failure prints what to try next rather than a stack
#  trace. See ARENA_LED.md §8 for what the bring-up steps are actually checking.
# ============================================================================
set -uo pipefail

ENV_NAME="${ARENA_ENV:-arenaled_d1mini32}"
PORT="${1:-}"
cd "$(dirname "$0")/.."

say()  { printf '\n\033[1;33m== %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m   %s\033[0m\n' "$*"; }
err()  { printf '\033[1;31m!! %s\033[0m\n' "$*" >&2; }

# --- PlatformIO present? ----------------------------------------------------
if ! command -v pio >/dev/null 2>&1; then
  err "PlatformIO (pio) not found."
  cat <<'EOF'
   Install it with:   pip install --user platformio
   or use the PlatformIO IDE extension in VS Code and run the tasks from there.
EOF
  exit 1
fi

# --- Port -------------------------------------------------------------------
if [ -z "$PORT" ]; then
  say "Looking for the board"
  pio device list || true
  # Pick a REAL USB-serial bridge. Taking "the first port" is wrong on macOS,
  # where the list always starts with cu.debug-console and cu.Bluetooth-* —
  # both are virtual, both have hwid "n/a", and flashing them just times out.
  # Rule: keep ports that report a USB VID:PID, and prefer the VIDs actually
  # used by ESP32 boards (CP210x, CH34x, FTDI, Espressif native USB).
  PORT=$(pio device list --json-output 2>/dev/null \
         | python3 -c 'import json,sys,re
try: d=json.load(sys.stdin)
except Exception: d=[]
KNOWN={"10C4":"CP210x","1A86":"CH34x","0403":"FTDI","303A":"Espressif","067B":"PL2303"}
cand=[]
for p in d:
    n,h=p.get("port",""),(p.get("hwid") or "")
    if "ttyS" in n and "USB" not in n: continue          # PC/VM motherboard UART
    if re.search(r"Bluetooth|debug-console|irda", n, re.I): continue
    m=re.search(r"VID:PID=([0-9A-Fa-f]{4})", h)
    if not m: continue                                    # no USB id -> not a board
    cand.append((0 if m.group(1).upper() in KNOWN else 1, n))
cand.sort()
if cand: print(cand[0][1])' 2>/dev/null || true)
fi

if [ -z "$PORT" ]; then
  err "No serial port found."
  cat <<'EOF'
   - Is the USB cable a DATA cable? Plenty of them are charge-only.
   - Linux: you must be in the 'dialout' group ->
        sudo usermod -aG dialout $USER   (then log out and back in)
   - Windows/macOS: install the driver for the board's USB bridge. Which one it
     is varies between D1 Mini ESP32 batches: CH340C on most, CP2104 on others
     (macOS names them /dev/cu.wchusbserial* and /dev/cu.usbserial-*). macOS 11+
     ships a CP210x driver already; only the CH340 needs installing.
   - Then re-run, or pass the port explicitly:
        tools/arena_flash.sh /dev/cu.usbserial-XXXX     (macOS)
        tools/arena_flash.sh /dev/ttyUSB0               (Linux)
EOF
  exit 1
fi
ok "port: $PORT"

UPLOAD_ARGS=(--upload-port "$PORT")
[ -n "${ARENA_SPEED:-}" ] && UPLOAD_ARGS+=(--project-option "upload_speed=${ARENA_SPEED}")

# --- Build ------------------------------------------------------------------
say "Building ($ENV_NAME)"
if ! pio run -e "$ENV_NAME"; then
  err "Build failed — nothing was written to the board, it is untouched."
  exit 1
fi

# --- Filesystem (web UI) first ---------------------------------------------
# Order matters: the firmware serves a cut-down fallback page when LittleFS is
# empty, so flashing the UI first means the very first boot already has it.
say "Uploading the web UI to LittleFS"
if ! pio run -e "$ENV_NAME" -t uploadfs "${UPLOAD_ARGS[@]}"; then
  err "LittleFS upload failed."
  cat <<'EOF'
   - Auto-reset can fail on CH340 boards: hold IO0 to GND, start this again,
     release IO0 once "Connecting..." turns into "Writing".
   - Or retry slower:  ARENA_SPEED=115200 tools/arena_flash.sh
   The firmware upload below can still succeed; you would just get the reduced
   fallback page instead of the full UI.
EOF
fi

# --- Firmware ---------------------------------------------------------------
say "Uploading the firmware"
if ! pio run -e "$ENV_NAME" -t upload "${UPLOAD_ARGS[@]}"; then
  err "Firmware upload failed."
  cat <<'EOF'
   - Hold IO0 to GND while the upload starts (this board has no BOOT button).
   - Close anything else holding the port (a serial monitor, Arduino IDE).
   - Retry slower:  ARENA_SPEED=115200 tools/arena_flash.sh
EOF
  exit 1
fi

cat <<EOF

$(ok "Flashed.")
   1. Join the WiFi network  'Arena-LED'   password 'pinball87'
   2. Single-LED bench test:
        http://192.168.4.1/api/set?count=1&mode=test
      -> the pixel must cycle RED -> GREEN -> BLUE -> WHITE, 2 s each.
   3. Full UI: http://192.168.4.1/
   4. Current checks (multimeter in the +5V wire):
        .../api/set?mode=classic&r=0&g=0&b=0&w=255&bright=255       ~20 mA
        .../api/set?mode=classic&r=255&g=255&b=255&w=255&bright=255 ~70 mA
        .../api/set?mode=off                                        ~1 mA

EOF

if [ "${ARENA_NO_MONITOR:-0}" = "1" ]; then
  exit 0
fi
say "Serial monitor (Ctrl-C to quit)"
pio device monitor -e "$ENV_NAME" --port "$PORT"
