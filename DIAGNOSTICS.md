# Diagnostics & service routes — GottFA80-PLuS ESP

Every HTTP route the board answers, what it is for, and how much damage it can do.
The board serves the same list, always in sync with the firmware actually running:

```sh
curl -s http://gottfa.local/routes            # grouped plain text
curl -s http://gottfa.local/routes?fmt=json   # machine-readable
```

**These routes are not clutter — they are the instruments this project was debugged
with.** `/sndtrace` is how a title's sound-command map was ground-truthed; `/ramsnap`
is how `$0072` (game in progress) and `$0109` (ball counter) were found by correlation
on a real Volcano. They stay. They are simply grouped and labelled so nobody mistakes
a bench tool for a feature.

---

## First thing to run

```sh
curl -s http://gottfa.local/sysinfo | jq
```

Identifies the board completely: firmware version + git commit + commit date, the
factory MAC (the one unforgeable board id), the running sketch's MD5, the FPGA IDCODE
it is talking to, WiFi mode/SSID/RSSI, heap, filesystem usage, SD/sound state.
**Quote this in any support request.** The version also shows in the web UI header,
on the OLED splash, on `/link`, and on the first serial line at boot.

---

## Group: product

Everyday use. The web UI drives these. Safe to hit at any time.

| Route | What it does |
|---|---|
| `/` | web UI (served from LittleFS) |
| `/ws` | WebSocket — live LISYcontrol state and commands |
| `/sysinfo` | board identity (above) |
| `/link` | FPGA UART telemetry: diag flag, game-in-progress, ball number |
| `/routes` | this list, from the running firmware |
| `/wifi`, `/wifi/*` | provisioning portal + its JSON API (see `WIFI_SETUP.md`) |
| `/snd` | PSOWAV: play a sound, load a game set, mixer statistics |
| `/game?id=N` | select a game's sound set by FPGA game number |
| `/roms`, `/fp` | ROM store contents; Free-Play variant selection |
| `/owned` | ownership gate: list / toggle / add |
| `/jobstatus` | poll a job started by one of the deferred routes |

## Group: diagnostic

Bench instruments. Read-only or momentary — but read the FPGA-reset warning below.

| Route | What it does |
|---|---|
| `/beep[?ms=800]` | 440 Hz sine straight to the PCM5102A. Hear it → I2S + DAC + wiring are good, so any WAV silence is the SD card. |
| `/sndtrace` | the sound-command capture ring. `?clear=1`, hit **one** playfield target, then read it back. `?fmt=json`, `?n=64`, `?raw=1`. This is how a title's map gets written. |
| `/ramsnap` | the whole 640-byte game RAM image (~1/s) as hex. Found `$0072` and `$0109`. |
| `/dispinj?ctrl=0x52&val=1234567` | drive the score glass by hand. Time-attack must be **disarmed** or the engine overwrites the control byte on its next tick. |
| `/jtag` | re-read the FPGA JTAG IDCODE. Refuses while an XVC session is live. |
| `/pin?n=14` | read a machine-wired input level (only GPIO 14 = FPGA reset, 8 = FPGA link). |
| `/led?r=..&g=..&b=..` | force the status LED to a colour — *which board in the rack is this?* The beacon takes the LED back on its next state change. |
| `/verify?path=…` | CRC32 a stored dump and look it up in the known-good ROM DB. |
| `/dump?type=…` | EPROM-reader daughterboard. Returns 503 unless `EPROM_READER_ENABLE` is set. |
| `/coiltest` | solenoid test by switch feedback — see below. `?do=learn`, `?do=test`, `?do=abort`, `?key=NN`. |

### `/coiltest` — which solenoid did not fire

A healthy solenoid **moves** something, and nearly everything that moves is watched by a
switch: the outhole kicker *opens* the outhole switch, a drop-target reset *closes* a whole
bank. So no current sensor is needed — only a per-machine table of which switch each of the
nine CPU-driven coils is supposed to disturb. Nobody has that table, so the machine builds
it once and replays it afterwards.

**On the machine, in this order.** Long-press the door TEST button (diag mode) → open the web
UI → **Bobines** tab → arm *mode contrôle* at the top → **Apprendre** (~25 s, once per title;
the ball tray should be in its normal resting state and nobody should touch the playfield) →
afterwards **Tester** (~5 s) any time. Signatures are saved to LittleFS as `/coilsig-NN.json`,
keyed by the FPGA game number; re-running *Apprendre* overwrites.

Per coil the test reports `ok`, `partiel`, `aucune réaction`, `non testable` (learn found no
switch that moves — knocker, chimes, coin lockout) or `contact inattendu`, plus any
`COIL_FAULT` the FPGA latched. **Coils marked "non testable" are a real blind spot, not a
pass** — check those by ear.

Learn pulses each coil three times and keeps a switch only if it reacted in at least two of
them, and each repetition is preceded by an idle control window of the same length so that a
direct-wired pop bumper firing on its own leaf switch is measured and subtracted instead of
being written into the signature. Flippers, bumpers and slingshots are wired direct on this
machine: they are never *tested*, but they can fire at any time and the control window is what
keeps them out of the results.

Both the route and the WebSocket commands return immediately; the pulsing runs as an
incremental state machine on the main loop (`coiltest.cpp`), so it never blocks AsyncTCP.
Poll `GET /coiltest` until `run` is 0. It refuses — out loud — unless diag mode is active
**and** the outputs are armed, and it aborts if either is lost mid-run.

### ⚠ These take the shared SPI bus — **they reset the FPGA**

`/nor`, `/sdprobe`, `/grant`, `/norhold`, `/norloop`, and all the `/nor*` service
routes assert the ESP's bus grant, which pulls the FPGA's reset line low. The FPGA
reboots and reloads the game ROM. **Any game in progress is lost.** Never run them
on a machine somebody is playing.

`/norhold` and `/norloop` used to *keep* holding those lines until someone remembered
`/norrelease` — one stray GET parked the pinball indefinitely. Since v1.0.0 both
**auto-release after 5 minutes** (`HOLD_MAX_MS` in `net.cpp`), which is far longer
than putting a probe on three legs takes. `/norrelease` still frees them immediately.

## Group: service — changes persistent state

**Not for a customer.** These write firmware, NOR flash, the ROM store, or the web UI
itself.

| Route | What it does |
|---|---|
| `/ota` | POST a firmware `.bin` → flash the other OTA slot and reboot. **Unsigned.** |
| `/fsup` | POST a file → LittleFS. This is how the whole web UI is replaced. |
| `/romup?id=N` | POST exactly 16384 bytes → encrypted into the ROM store. |
| `/romdel?id=N` | delete a game slot. |
| `/wavup?dir=&name=` | POST a WAV → the sound SD card. |
| `/norflash?addr=0x…` | POST an image → erase + program + verify into the W25Q32. |
| `/norwrite` | NOR self-test on the **last** 4 KB sector (`0x3FF000`). |
| `/norbig` | NOR self-test on a whole 16 KB game slot (`0x3F0000`). |

Anything slow (`/nor`, `/norbig`, `/norwrite`, `/norflash`, `/sdprobe`, `/grant`,
`/verify`) answers **202** with a job id and runs on the main loop, never inside the
AsyncTCP handler. Poll it:

```sh
curl -s esp/norbig            # {"job":3,"poll":"/jobstatus?id=3"}
curl -s 'esp/jobstatus?id=3'  # poll until "state":"done", then read "result"
```

Only the most recent job is remembered, and only one runs at a time — they all drive
the same SPI bus.

## Not HTTP: TCP 2542 — XVC (JTAG over WiFi)

Reflashes the **FPGA** with no programming cable:

```sh
# load into FPGA RAM (lost at power-off, ideal for trying a build) — must be .svf
openFPGALoader --cable xvc-client --ip <board> --port 2542 SYS80.svf

# permanent EPCS burn (~6-8 min) — input is the .rbf
caffeinate -dimsu openFPGALoader --cable xvc-client --ip <board> --port 2542 \
           -f --verify SYS80.rbf
```

A `.rbf` sent to SRAM reports "Done" but the design never starts — use `.svf` there.
`caffeinate` is not optional on a Mac: if the laptop sleeps mid-burn the WiFi drops,
the write fails half-done and the FPGA will not configure until it is burned again.

---

## Security — read this before putting a board on a shared network

**Every route above is unauthenticated, and so is the XVC port.** Anyone who can reach
the board's IP can reflash the ESP (`/ota`, unsigned), replace the web UI (`/fsup`),
reprogram the NOR, or reconfigure the FPGA. Several destructive routes are plain `GET`,
so a bookmark, a browser prefetch or a crawler can trigger them.

For v1 this is **documented, not fixed** — adding authentication before the whole
firmware has been validated on hardware risks locking the owner out of a board with no
console. Until it is fixed:

- keep the board on a trusted home network, or leave it on its own SoftAP;
- change the hotspot password from the portal — the factory one (`pinball80`) is the
  same on every board and is published in `WIFI_SETUP.md`. `/sysinfo` reports
  `"apPassDefault":true` while it is still the factory value;
- do not port-forward the board, and do not expose port 80 or 2542 to the internet.

The intended fix, in order: HTTP Basic auth (or a token) on the *service* group, `POST`
instead of `GET` for anything destructive, a signed OTA image, and a per-board AP
password printed on a label at build time.
