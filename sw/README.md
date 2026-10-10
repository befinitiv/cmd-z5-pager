This document proposes the software architecture for the CMD-Z5 replacement board described in `../board/README.md`. The firmware turns the phone into a standalone Meshtastic and LoRaWAN (TTN) messenger with its own UI and predictive text input. The same code also builds for the PC, where an emulator window stands in for the phone hardware.

Status: phases 1 to 3 are implemented (board definition, emulator, graphics, UI toolkit, menus, hardware test; predictive text with language models for German and English; Meshtastic messaging, tested in the emulator). See [Build and run](#build-and-run) and [Development plan](#development-plan).

# Decisions

| Topic | Decision | Reason |
|---|---|---|
| OS / SDK | Zephyr through the nRF Connect SDK (NCS) | Ready-made drivers for nRF52840, SX1262, nPM1300 (regulators, charger, fuel gauge), LIS2DH12, QDEC, u-blox M10 (GNSS), keypad matrix and USB. Also gives a LoRaWAN stack, BLE, LittleFS/settings, MCUboot and the `native_sim` PC target |
| Meshtastic | Own implementation of the over-the-air protocol, no fork of the Meshtastic firmware | The Meshtastic firmware fills most of the 1 MB flash and has no LoRaWAN. We need only the radio protocol, not the full feature set |
| Phone app (BLE PhoneAPI) | Not in the first version | The architecture leaves room for it (see [Later extensions](#later-extensions)) |
| Predictive text languages | German and English, switchable at runtime | |
| Language model storage | Internal flash, in its own partition | An external SPI NOR flash may come with board rev 2. The model reader only sees a flash area, so the model can move there without code changes |
| PC build | Zephyr `native_sim` with an SDL2 "front panel" | The same kernel, threads, timers, zbus, settings and file system run on the PC. Only the lowest driver layer is swapped |

Assumptions to confirm:

* **Region:** EU868 for both Meshtastic (`EU_868`) and LoRaWAN (TTN EU1).
* **LoRaWAN use case:** periodic uplinks of position and battery, user-triggered text uplinks, and downlinks shown as messages in the inbox.
* **Meshtastic version:** the firmware targets a pinned protobuf release (2.5 or newer, with PKI direct messages).

# Overview

```
┌──────────────────────────────────────────────────────────────────────────┐
│ App / UI   screens: home, inbox, chat, compose, nodes, map/GPS,          │
│            LoRaWAN, settings  ─  screen stack, widgets, status bar       │
├───────────────┬──────────────────────┬───────────────────────────────────┤
│ libtextinput  │ libgfx (2bpp FB,     │ Services (Zephyr threads,         │
│ multitap, T9, │ fonts, icons)        │ connected through zbus)           │
│ n-gram LM,    │                      │  mesh · lorawan · radio arbiter   │
│ user model    │                      │  gps · power · notify · msgstore  │
├───────────────┴──────────────────────┴───────────────────────────────────┤
│ libmeshtastic: protobuf (nanopb), crypto, router, nodedb, port modules   │
├──────────────────────────────────────────────────────────────────────────┤
│ Zephyr device APIs:  input · led · pwm · lora · lorawan · regulator ·    │
│ gnss · charger · fuel gauge · sensor; own: lcd, lcd_bus, audio_out       │
├────────────────────────────────┬─────────────────────────────────────────┤
│ cmdz5 board (nRF52840)         │ native_sim + emulator (SDL2)            │
│ bit-banged LCD bus, gpio-kbd-  │ LCD controller model, PC keyboard and   │
│ matrix, QDEC, PWM audio,       │ mouse wheel, green backlight, 2 audio   │
│ SX1262, nPM1300, LIS2DH12, M10 │ sinks, vibration, radio/GPS/PMIC stubs  │
└────────────────────────────────┴─────────────────────────────────────────┘
```

Rules that keep both builds identical above the driver layer:

1. Application code uses only Zephyr device APIs (`input`, `led`, `lora`, `gnss`, `regulator`, …) and three small custom APIs (`lcd`, `lcd_bus`, `audio_out`). It never touches nRF registers or SDL.
2. The board devicetree and the `native_sim` overlay declare the same node labels (`lcd`, `backlight`, `vibration`, `speaker`, `earpiece`, later `lora0`, `gnss`, `pmic`, …), so `DEVICE_DT_GET(DT_NODELABEL(lcd))` works on both.
3. `libtextinput`, `libgfx` and most of `libmeshtastic` are plain C with no Zephyr dependency. They also build as host unit tests and as a Python extension for the language model tools.

# Directory structure

Entries marked *(later)* do not exist yet.

```
sw/
├── CMakeLists.txt  Kconfig  prj.conf
├── boards/
│   ├── befi/cmdz5/                  # board definition (HWMv2): dts, pinctrl, defconfig, flash partitions
│   ├── cmdz5_nrf52840.conf          # board-only Kconfig (I2C, PWM, regulators, charger)
│   └── native_sim_native_64.overlay # emulator: same node labels, emulated devices
├── dts/bindings/                    # sony,cmdz5-lcd, cmdz5,lcd-bus-gpio, cmdz5,audio-pwm, emulator nodes
├── include/
│   ├── cmdz5/drivers/               # lcd.h, lcd_bus.h, audio_out.h (custom device APIs)
│   └── dt-bindings/cmdz5/keys.h     # key codes without a Zephyr equivalent (CALL, Memo, *, #, lid)
├── drivers/
│   ├── display/cmdz5_lcd.c          # controller driver (init sequence, row writes, contrast, power)
│   ├── display/lcd_bus_gpio.c       # 8080 bus bit-bang, one masked port write per byte on P0.19–P0.26
│   └── audio/audio_pwm.c            # tones on PWM; PCM via EasyDMA (later)
├── emul/                            # native_sim only
│   ├── frontpanel.c                 # Zephyr side: input device, state, render thread
│   ├── frontpanel_bottom.c          # host side: SDL2 window, drawing, keyboard/mouse, audio
│   ├── lcd_bus_emul.c               # implements lcd_bus, feeds lcd_ctrl_model
│   ├── lcd_ctrl_model.c             # model of the display controller: registers 03/04/0B → 96×72 2bpp FB
│   ├── led_emul.c  audio_emul.c     # backlight/vibration and speaker/earpiece
│   ├── script.c                     # --script: scripted keys, wheel, lid and screenshots
│   ├── radio_emul.c                 # mesh radio over UDP multicast between emulator instances
│   ├── gnss_emul.c                  # (later) fixed position or NMEA file replay
│   └── battery_emul.c               # simulated battery: current from the emulated outputs, USB from the front panel
├── lib/
│   ├── gfx/                         # 2bpp framebuffer, primitives, fonts, icons, dirty-row tracking
│   ├── ui/                          # screen stack, events, menu, slider, text view
│   ├── textinput/                   # model reader, prediction engine, user model, editor
│   └── meshtastic/                  # packet codec, crypto (AES, CCM, SHA-256, X25519), channels, router, nodedb
├── src/
│   ├── main.c                       # UI thread and event loop
│   ├── input.c                      # Zephyr input events → UI events, long press
│   ├── hw.c                         # display, backlight, vibration, tones and melodies
│   ├── power.c                      # backlight and display timeouts, lid, power key
│   ├── battery.c                    # battery sampling (1 s), current averages; backends battery_npm13xx.c / emul
│   ├── i18n.c                       # menu language (English, German)
│   ├── textin.c                     # models, engines and user models per language
│   ├── clock.c  config.c  statusbar.c
│   ├── mesh/                        # mesh service (router thread, identity, nodes) and message store
│   ├── services/                    # (later) radio_arbiter, lorawan_svc, gps_svc, notify_svc
│   └── screens/                     # home, menu, settings, hardware test, power off, compose, conversations, chat, nodes; (later) inbox, chat, compose, nodes
├── lm/                              # language models de.lm, en.lm (+ sources and licences)
├── modules/lm_image/                # Zephyr module: model pack for the lm partition, merged.hex
├── tools/
│   ├── fontconv/                    # BDF → C font headers (X11 misc-misc fonts, public domain)
│   ├── lmbuild/                     # corpus → n-grams → prune/quantise → .lm; evaluation
│   └── lcd_replay/                  # (later) replays ../display .dsl captures through lcd_ctrl_model
└── tests/                           # emul/ (smoke.txt, run_mesh.sh), textinput/ and meshtastic/ (host tests: run.sh)
```

# Hardware abstraction

| Function | Zephyr API / driver (target) | Emulator |
|---|---|---|
| Display 96×72, 2 bpp | `lcd` API → `cmdz5_lcd` → `lcd_bus` (GPIO bit-bang), supply from nPM1300 LDSW1 | `lcd_bus_emul` → `lcd_ctrl_model` → SDL texture, scaled 5× |
| Keypad 4×5 matrix | `input` subsystem, `gpio-kbd-matrix` (supports idle wake through GPIO SENSE) | PC keys and clickable keys on the front panel → `input_report_key()` |
| Rotary wheel | `gpio-qdec` → `INPUT_REL_WHEEL` (GPIO interrupts while idle, so no QDEC current) | mouse wheel, Up/Down keys → `INPUT_REL_WHEEL` |
| Wheel push, bend up/down | keypad matrix row 4 | Space or middle click, Left/Right or PgUp/PgDn (and screen buttons) |
| Lid, power key | `gpio-keys` → `CMDZ5_KEY_LID` (Zephyr has no switch events), `INPUT_KEY_POWER` | `L` key toggles lid (keypad drawn dimmed while closed), `Esc` is the power key |
| Green LEDs (keypad and board) | `led` API on PWM (P0.08), so brightness and fade work | window and keypad area tinted green with the LED brightness |
| Loudspeaker | `audio_out` → PWM EasyDMA on P0.27 (FET, square wave or PCM) | SDL audio, full volume |
| Earpiece | `audio_out` → PWM EasyDMA on P0.06 → RC → PAM8302 (enabled by LDSW2) | SDL audio, quieter sine, EAR indicator lit |
| Vibration | `led` API on PWM (P0.07) | display shakes, VIB indicator lit |
| LoRa SX1262 | `lora` API (`semtech,sx1262`), power via nPM1300 BUCK1 | stub: TX logged and RX injectable; optional UDP multicast between instances |
| GPS M10N | `gnss` API (u-blox M10 driver), 5 V boost EN on P1.03 | fixed position from config, or NMEA file replay |
| PMIC nPM1300 | `regulator`, `charger` sensor (voltage, current, die temperature, charge and VBUS state), nRF Fuel Gauge | simulated battery (`battery_emul.c`): current from the emulated outputs, USB power from the front panel |
| Accelerometer | `sensor` API (`st,lis2dh`), motion trigger | key `A` sends a motion event |
| Microphone | not used in the first version | – |

Three custom APIs are added because Zephyr has nothing suitable (Zephyr's `display` API has no 2 bpp gray format):

* **`lcd`:** `write_rows(fb, first, count)`, `set_contrast()`, `set_power()`. The framebuffer is in the controller's native format, so rows go out without conversion. The application only sends rows that changed.
* **`lcd_bus`:** `write(a0, buf, len)`, `reset()`, `enable()`. The controller driver (`cmdz5_lcd`) sits above it and is shared by target and emulator. The emulator therefore runs the real init sequence and row-addressing code against a model of the controller, which logs a warning for anything the reverse-engineered documentation does not cover. A bug in the driver shows up on the PC before the board exists. `tools/lcd_replay` (later) will feed the logic analyzer captures through the same model.
* **`audio_out`:** `tone(freq, ms, volume)`, `stop()`, `busy()`, one device per output (`speaker`, `earpiece`). PCM playback through PWM EasyDMA follows later. A melody player sits above it in `src/hw.c`; an RTTTL sequencer for ringtones and notifications follows in `notify_svc`. Meshtastic uses RTTTL for its external notification module.

## Emulator details

`native_sim` builds the firmware as a Linux executable. SDL calls must run on the host side of the native simulator, so the front panel is split into a Zephyr-side part (`frontpanel.c`) and a host-side part (`frontpanel_bottom.c`, built against SDL2). This is the same split that Zephyr's own `display_sdl` uses. The SDL event loop is Zephyr's (`boards/native/common/sdl`); the front panel hooks in with `SDL_AddEventWatch`.

Front panel layout: the display at the top (scaled 5×, with the gray levels mixed into the panel colour, which turns green with the backlight), the keypad below it as clickable buttons whose labels glow green with the keypad LEDs, and on the right the wheel, LED/VIB/SPK/EAR/USB indicators, lid switch and power key. Key mapping:

| PC | Phone |
|---|---|
| `0`–`9` (main row or numpad) | digit keys |
| `+` or numpad `*` / `#`, `\` or numpad `/` | `*` / `#` |
| `Enter` | CALL |
| `Backspace` | C |
| `M` | Memo |
| mouse wheel, `Up` / `Down`, click on the wheel | wheel rotate |
| `Space`, middle click | wheel push |
| `Left` / `PgUp` | wheel bend up (back) |
| `Right` / `PgDn` | wheel bend down |
| `L` | toggle the lid |
| `U`, click on the USB indicator | plug or unplug USB power (charges the simulated battery) |
| `Esc` | power key: short = display on/off, hold 1.5 s = switch off; while off, hold 1 s = switch on (restarts the emulator) |
| `F12` | save the display as `cmdz5-screen-NNN.bmp` |

The lid covers only the keypad; the display stays visible. Closing it returns to the home screen and switches the backlight off. Keypad keys are ignored while it is closed, the side controls (wheel, push, bend, power) still work.

Settings are stored in `flash.bin` in the working directory (Zephyr's flash simulator).

After a restart (switching on again) the emulator starts with the same arguments, so a `--script` runs again.

`--script=<file>` runs front panel commands (`wait`, `key`, `press`, `release`, `wheel`, `lid`, `usb`, `shot`, `quit`, see `emul/script.c`) for UI tests and screenshots. With `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy` it runs without a window and sound, e.g. in CI. `tests/emul/smoke.txt` walks through all current screens.

**Emulated radio** (`emul/radio_emul.c`): packets travel as UDP multicast on the loopback interface, so emulator instances on the same machine form a mesh. Each packet carries the radio settings (frequency, bandwidth, spreading factor, sync word); only instances with the same settings receive it. Sending takes the packet's time on air, and the radio is half-duplex. Options: `--node-num=<hex>`, `--radio-port=<n>` (default 4403), `--radio-off`. Run each instance in its own directory, because the settings (and so the node identity) are stored in `flash.bin` in the working directory:

```
mkdir -p /tmp/a /tmp/b
(cd /tmp/a && ~/repos/cmd-z5/sw/build-sim/zephyr/zephyr.exe --node-num=aaaa0001) &
(cd /tmp/b && ~/repos/cmd-z5/sw/build-sim/zephyr/zephyr.exe --node-num=bbbb0002) &
```

Planned option: `--gps lat,lon`.

# Concurrency and services

All services are Zephyr threads or work-queue items and communicate only through **zbus** channels. The UI never blocks on the radio, and every service can be tested on its own.

| Channel | Producer → consumer | Content |
|---|---|---|
| `input_evt` | input subsystem → UI | key, wheel, lid, power |
| `msg_rx` | mesh_svc, lorawan_svc → msgstore, notify, UI | received text and data |
| `msg_tx` | UI → mesh_svc / lorawan_svc | outgoing message with a tracking ID |
| `msg_status` | mesh_svc → UI | queued, sent, acked, failed |
| `node_update` | mesh_svc → nodestore, UI | NodeInfo, position, SNR, last heard |
| `gps_fix` | gps_svc → mesh_svc, lorawan_svc, UI | position, time |
| `power_state` | power_svc → all | active, dim, screen off, deep sleep; battery level, charging |
| `radio_state` | radio_arbiter → UI | current owner, duty-cycle budget |
| `notify_req` | any → notify_svc | pattern for LEDs, vibration and sound |

## Radio arbiter (Meshtastic and LoRaWAN on one SX1262)

The SX1262 can only listen on one frequency and sync word at a time. Meshtastic needs continuous RX (sync word 0x2B, 869.525 MHz for `EU_868`). LoRaWAN class A only needs the radio for TX and the RX1/RX2 windows (sync word 0x34, 868.1–868.5 MHz, RX2 at 869.525 MHz SF9).

* **Default owner:** Meshtastic, with the radio in continuous RX.
* **LoRaWAN lease:** before an uplink, `lorawan_svc` asks the arbiter for a lease. The arbiter waits until no Meshtastic packet is being received or sent, stops RX, hands the radio to the Zephyr LoRaWAN stack for TX + RX1 + RX2 (about 3 s), then restores the Meshtastic configuration. Meshtastic packets sent during the lease are lost, but the mesh's rebroadcasts and retries usually cover that.
* **Duty cycle:** the arbiter keeps one airtime budget per EU868 sub-band for both protocols (1 % for 868.0–868.6 MHz, 10 % for 869.4–869.65 MHz).
* **Risk:** the Zephyr LoRaWAN subsystem (LoRaMac-node) assumes it owns the radio. Prototype switching between raw `lora` RX and `lorawan_send()` early, on real hardware. If it fails, the fallback is a time-sliced mode with LoRaWAN uplinks only at fixed intervals and the LoRaMac stack paused in between.

## Meshtastic (`lib/meshtastic`, `src/mesh`)

Implemented in phase 3: the over-the-air protocol of current Meshtastic firmware (2.6/2.7). The protocol details were taken from the Meshtastic sources (header layout, nonces, channel hash, frequency slot, presets, flooding rules); no Meshtastic code or generated protobuf code is used, so their GPL licence does not apply to this firmware.

| Module | Content |
|---|---|
| `packet` | 16-byte header (to, from, id, flags with hop limit, want-ack and hop start, channel hash, next hop, relay node); a hand-written protobuf codec for Data, User (NodeInfo), Position and Routing |
| `crypto` | AES-128/256, CTR, CCM, SHA-256, X25519 in plain C. Channel messages: AES-CTR with the channel key, nonce = packet id (64-bit LE) + sender. Direct messages ("PKI"): AES-256-CCM with SHA-256 of the X25519 secret, 8-byte tag + 4-byte extra nonce. Current Meshtastic firmware refuses to send direct messages to nodes without a public key, so PKI is needed to receive DMs |
| `channel` | key expansion (one-byte PSK index → default key), channel hash (XOR of name and key), frequency slot (djb2 of the channel name), presets LongFast … ShortTurbo, regions EU_868, US, EU_433, slot time, time on air |
| `router` | managed flooding: duplicate history, rebroadcast with hop_limit − 1 after an SNR-weighted delay (distant nodes first), cancelled when another node relays first; ACKs for `want_ack`, 3 retries, implicit ACK for broadcasts (hearing a relay), re-ACK of retries, NAK handling; PKI for direct text messages when the key is known, else the channel key |
| `nodedb` | 48 nodes: user info and public key, position, last heard, SNR, RSSI, hops; least recently heard replaced |

The mesh service (`src/mesh/mesh.c`) runs the router in its own thread:

* **Identity:** node number from the chip's device id (emulator: random or `--node-num`), X25519 key pair from the entropy source, default names "CMD-Z5 xxxx" / "xxxx"; all kept in the settings.
* **NodeInfo:** broadcast 5–15 s after the start and every 3 hours; answered when requested; a node that sends something without being known gets our NodeInfo with a request for its own (rate-limited), so keys are exchanged quickly.
* **Messages** (`msgstore.c`): the primary channel and one conversation per node, the newest 40 messages, delivery status queued → sent → delivered / no relay / failed. They are stored in LittleFS (`msglog.c`, `/lfs/msgs.log`): new messages, status changes and read marks are appended as small records and replayed at startup; above 24 KB the log is rewritten with the current messages only. Messages that were still queued when the device stopped are shown as failed.
* **Nodes** with user info and position are stored in the settings and survive a restart.
* **New messages:** sound, vibration and the display wakes up. The home screen shows a preview; on other screens a pop-up (`ui_toast()`) shows the sender and the start of the text for 8 s, unless that conversation is open. Push opens the conversation; any other key only closes the pop-up.

Tested on the host (`tests/meshtastic/run.sh`): crypto against FIPS-197 and RFC 7748 vectors and against vectors generated with Python `cryptography`; the LongFast channel hash (8) and the US LongFast frequency (906.875 MHz) as seen on real meshes; router scenarios in a simulated network (two-hop flooding, implicit ACK, encrypted DM over two hops with ACK, fallback to the channel key, retries until failed, lost ACK, relay cancelled in a triangle, foreign channel relayed but not delivered). In the emulator, `tests/emul/run_mesh.sh` lets two instances exchange a channel message and an encrypted direct message. A test against a stock Meshtastic node needs the board (phase 5).

Not yet: secondary channels, position broadcasts (no GPS yet), telemetry, traceroute, next-hop routing (packets are flooded), duty-cycle accounting, the PhoneAPI.

## LoRaWAN (`lorawan_svc`)

Uses the Zephyr LoRaWAN subsystem (OTAA, EU868, class A). DevEUI, JoinEUI and AppKey are entered in the settings or over the USB shell and stored with the settings subsystem. The DevNonce is kept persistently because LoRaWAN 1.0.4 requires it. Uplinks: Cayenne LPP for position and battery (so TTN decodes them without a custom formatter), and port 10 for UTF-8 text. Downlinks appear in the inbox as a "TTN" conversation.

## Power (`power_svc`)

| State | Display | Backlight | LoRa | GPS | Typical entry |
|---|---|---|---|---|---|
| Active | on | on | RX | per setting | key press, lid opened |
| Dim | on | off | RX | per setting | light timeout (default 10 s, can be "never"), lid closed (also returns to the home screen) |
| Screen off | off (LDSW1 off) | off | RX | per setting | power key, screen timeout (default never) |
| Low power | off | off | off (BUCK1 off) | off | user choice, or low battery |
| Off (ship mode) | – | – | – | – | power key held 1.5 s and released; critical battery (later). Hold the power key about 1 s to switch on again (`ship-to-active-time-ms`). With USB connected the PMIC stays awake and the firmware restarts instead |

`power_svc` enforces the rules from the board README: drive or float the signal lines of switched-off parts, rows on SENSE, columns low. Waking in low-power mode comes from keys, lid, power key, accelerometer or RTC (for scheduled LoRaWAN uplinks). Zephyr device runtime PM switches off the drivers; the nPM1300 regulators are switched through the `regulator` API. Charger setup (current, termination voltage, NTC disabled) happens early in boot, because the nPM1300 starts with charging disabled.

### Battery and current measurement (`src/battery.c`)

A thread samples the battery once a second. On the board the nPM1300 charger driver measures battery voltage, current and die temperature (the battery has no NTC, so the die temperature stands in for it), plus the charge and VBUS state. nRF Fuel Gauge turns these into the state of charge and the times to empty and full; its state is stored in the settings (`batt/fg`) every 6 hours and before switching off. The emulator simulates the battery instead (the library exists only for ARM): an assumed 1000 mAh cell whose current follows the emulated outputs with the estimates below (radio RX 4.5 mA, TX 110 mA, backlight 15 mA, vibration 80 mA, speaker 60 mA, earpiece 15 mA; the display is not counted), quantised like the nPM1300 ADC.

The current resolution follows the discharge limit in the devicetree: full scale is 1.12 × the limit, 10 bits. With the 1 A limit one step is 1.1 mA, so single readings of the 4.5 mA receive current are coarse; the averages over a minute and an hour smooth this out. The 200 mA limit would give 0.22 mA steps, but the battery could then not supply transmit, loudspeaker, vibration and backlight at the same time. Currents in the µA range (low-power mode) are below the resolution and need an external meter such as the PPK2.

**Diagnostics** (main menu) shows charge, voltage, supply (battery, USB charging, charged, charger error), the latest current, the averages over the last minute and hour, the charge drawn since the start, the time to empty or to full, the temperature and the uptime. The status bar shows the charge and a lightning bolt while USB power is present.

**Battery model:** `src/battery_model.inc` is Nordic's example model from the NCS nPM1300 fuel gauge sample, not the cell in the phone. For a correct state of charge, profile the real cell with the nPM1300 EK and nPM PowerUP and replace the file.

## Storage

| Data | Mechanism | Location |
|---|---|---|
| Settings, node identity and keys, known nodes, learned words | Zephyr settings on NVS | `storage` partition (32 KB) |
| Messages | LittleFS, append-only log | `lfs` partition (96 KB) |
| Language models | read-only pack, memory-mapped | `lm` partition (352 KB; later on external flash) |

# User interface

## Display and graphics (`lib/gfx`)

The framebuffer is 96 × 72 pixels at 2 bpp = 1728 bytes, in the native format of the display (24 bytes per row, MSB = leftmost pixel). `gfx` keeps a dirty bit per row, and `cmdz5_lcd` only sends changed rows (29 bytes each). Fonts: a proportional 5×7 font (about 18 characters per line) for text, a 3×5 font for the status bar, a large digit font for the clock, all with German characters. The 4 gray levels are used for anti-aliasing, selection bars and disabled items.

LVGL was considered and rejected: it costs 100+ KB of flash and does not fit this display.

## Layout

The status bar (battery, mesh/LoRaWAN state, GPS, unread messages, time; 7 px) is shown only on the home screen. All other screens use the whole display, so menus show 7 rows and the compose screen 7 lines of text.

```
┌────────────────────────────┐  compose screen
│T9 Abc DE             17/200│  y 0–6    input mode, language, length
│Ich gut bin Mate|           │  y 7–62   text (7 lines at 8 px), word being typed
│                            │           underlined, completion part in gray
│                            │
├────────────────────────────┤
│[gut] Hut gute guten        │  y 63–71  candidate bar (wheel selects, push accepts)
└────────────────────────────┘
```

## Toolkit (`lib/ui`)

A small retained-mode toolkit: a screen stack with `on_enter`, `on_event`, `render` and `on_exit`, and widgets for list/menu, text view with wheel scrolling, text field, candidate bar, number/hex entry, toggle and progress bar. Screens subscribe to zbus channels and redraw only when data changes.

## Key roles

| Key | Navigation | Text entry |
|---|---|---|
| Wheel rotate | move selection, scroll | move through the candidates (also into the next-word predictions); in cursor mode: move the cursor (up = back) |
| Wheel push | select / open | accept the selected candidate, with a space |
| Wheel bend up | back (long: home screen), same as C | back; the draft is kept |
| Wheel bend down | page down | still open |
| CALL | send / confirm | send message (phase 3) |
| C | back (long: home screen) | delete a key or the character before the cursor (long: the word before it); back when the text is empty |
| Memo | context menu | symbol and smiley picker |
| `2`–`9` | select menu entry | add a key to the word (long: the digit) |
| `#` | – | input mode: T9 → multitap → 123 (long: input language) |
| `*` | – | cursor mode on/off: the cursor blinks and the wheel moves it (typing or push ends it); long: case Abc / abc / ABC |
| `0` | – | accept the word and add a space (long: `0`) |
| `1` | – | punctuation `. , ? ! ' - : ; ( ) @ /`, press again for the next one; replaces the space after an accepted word |

In text entry the keys act when released, so that a long press can do something else.

## Menu language

The menus are in English or German (Settings → Language, stored with the other settings). Strings stay English in the code and are translated when drawn: `tr("Settings")` in the application, and the UI library passes titles, menu labels and its own texts through the same hook (`ui_set_translator()`). The German table is in `src/i18n.c`; a missing entry falls back to English. Texts must stay short: a menu row holds about 18 characters.

## Screens

Home (clock, battery; with unread messages a preview: sender, count and the start of the newest text) · Inbox (channels, DMs, TTN) · Chat (scrollback with delivery state: queued / sent / acked) · Compose · Nodes (list sorted by last heard, detail with distance and bearing from GPS, SNR, hops; start a DM or traceroute) · GPS status · LoRaWAN (join state, last uplink, send now) · Diagnostics (battery, current consumption) · Settings (Factory reset: erases all settings and messages and restarts · Mesh: name, region, preset, channels and PSK · LoRaWAN: keys, interval · Display: contrast, backlight timeout · Sound: ringtone, vibration, earpiece/speaker · Input: languages, learn words on/off, reset dictionary · Power · About).

# Predictive text (`lib/textinput`)

Implemented in phase 2. `lib/textinput` is plain C without Zephyr or dynamic memory; the same code runs on the board, in the emulator, in the host tests and, through ctypes, in the Python evaluation.

## Input methods

* **Predictive (T9):** one key press per letter. The candidate bar shows the exact matches and completions (longer words) ranked by context; the word being typed shows the first candidate, with the not yet typed part of a completion in gray. German letters map to their base key (ä→2, ö→6, ü→8, ß→7).
* **Next-word prediction:** after a space the bar shows the likely next words. Turning the wheel selects one, push accepts it.
* **Multitap (abc):** classic letter cycling with a 1 s timeout, including ä ö ü ß on the German keys. Words spelled this way are learned and offered in predictive mode from then on.
* **Numeric (123)**, **punctuation** on key 1 and a **symbol picker** (Memo) with accented letters and smileys. While a key 1 character is pending, the bar at the bottom lists all its choices with the current one highlighted, like the word suggestions; pressing 1 again or turning the wheel moves through them, push accepts.
* **Case:** Abc (capital at the start of a sentence), abc (as typed or as in the lexicon, so German nouns keep their capital), ABC.
* **Cursor:** `*` switches the wheel from the candidates to moving the cursor through the text (the cursor blinks). Text is inserted and deleted at the cursor; predictions use the words before it.

## Engine

For each key press `ti_predict()` collects candidates and ranks them:

1. **Lexicon lookup:** the key sequence gives an id range: exact matches first, then all completions.
2. **Context:** cost `-log2 P(w | w-2, w-1)` from the trigram model with backoff to bigrams and unigrams (absolute discounting with backoff weights). This is where the previous words come in: after "Mir geht es" the keys `488` give "gut", after "Ich trage einen" they give "Hut".
3. **Candidates for completions:** the successors of the context that fall into the key range, plus the 24 most frequent words of the range.
4. **User model:** counts of the words and word pairs the user typed (64 words, 128 pairs, about 3.6 KB per language, stored in the settings when the compose screen is left). Unknown words are stored with their spelling.
5. **Conversation cache:** the last 48 words of the conversation get a boost.
6. **Mixture:** `P = 0.82 P_lm + 0.08 P_user(w | w-1) + 0.05 P_user(w) + 0.05 P_cache(w)`. Completions get a penalty of 5 bits against exact matches (tuned so that the first shown word is almost as often right as with exact matches only).

## Model format and size

Built offline per language by `tools/lmbuild` (`build_models.sh` downloads the corpora and rebuilds both models; `build.py` and `evaluate.py` can also be run on their own):

1. Corpus: Tatoeba (short conversational sentences) and the Leipzig web corpus, see `lm/README.md` for sources and licences. Tatoeba's example names Tom, Mary and Maria appear in up to 23 % of its sentences and are downsampled.
2. Tokenise; numbers and unusable tokens break the context, commas are transparent. Sentence-initial words are truecased; case variants are separate words if each is common (German "Sie" and "sie").
3. Vocabulary: the 20 000 most frequent words (web junk and single letters other than "a"/"I" removed).
4. Count unigrams, bigrams and trigrams (numpy), estimate with absolute discounting, prune by count-weighted change of log probability until the byte budget is reached (30 % of the n-gram budget for trigrams).
5. Encode (format in `tools/lmbuild/lmformat.py`, reader in `lib/textinput/src/model.c`):
   * lexicon sorted by key sequence, front-coded in blocks of 32 words with canonical Huffman codes for the shared prefix length, the letters and the case: 24–26 bits per word;
   * unigrams as 6-bit costs;
   * bigram and trigram tables with Rice-coded id gaps, 4-bit quantised costs and backoff weights (Lloyd codebooks), anchors every 16 contexts; a trigram context is the index of its bigram: 20 and 25 bits per entry.

| Part (per language) | German | English |
|---|---|---|
| Lexicon (20 000 words) | 64 KB | 60 KB |
| Unigrams | 15 KB | 15 KB |
| Bigrams | 70 KB (28 483) | 73 KB (29 838) |
| Trigrams | 30 KB (9 763) | 31 KB (10 086) |
| **Total** | **176 KB** | **176 KB** |

## Evaluation

`evaluate.py` simulates typing 2 000 held-out sentences with the C engine. Keystrokes per character (KSPC; each word's space counts as a character and a key):

| Method | German | English |
|---|---|---|
| Multitap | 1.97 | 1.93 |
| Classic T9 (frequency only) | 1.02 | 1.01 |
| T9 with context | 1.01 | 1.01 |
| **Full: context + completions + next-word prediction** | **0.68** | **0.69** |
| First exact match correct, frequency only | 90.5 % | 94.8 % |
| First exact match correct, with context | 93.6 % | 97.0 % |
| Words picked before pressing a key | 21.7 % | 25.9 % |
| Words not in the lexicon (OOV) | 5.0 % | 1.1 % |

"Full" assumes the user picks a word from the first three candidates as soon as it appears there. Context removes about a third (German) to 40 % (English) of the wrong first guesses of classic T9. The user model cannot be measured this way (the held-out text is not one person's writing); it helps with names and the user's own phrases.

## Flash budget (1 MB internal)

| Partition / part | Size |
|---|---|
| MCUboot (single slot, USB serial recovery) | 48 KB |
| Application: Zephyr, drivers, LoRaWAN, Meshtastic + crypto, UI, fonts, text engine | 380–450 KB |
| Settings + LittleFS | 128 KB |
| Language models, German + English | 352 KB (2 × 176 KB) |
| Reserve (BLE stack for the PhoneAPI needs about 120 KB) | rest |

On the board the models live in the `lm` partition as a pack (`tools/lmbuild/pack.py`: a small directory of language, offset and size, then the models), read in place from the memory-mapped flash; the application itself is 143 KB after phase 3. The two models fill the 352 KB partition up to 868 bytes. The emulator links the models into the program. The budget is tight. `lmbuild` takes the partition size as a target and prunes until the model fits. The `lm` partition can be updated separately from the application over USB (mcumgr). With an external SPI NOR flash in rev 2, the models move there and can be 5–10 times larger (bigger vocabulary, full trigrams, optionally a small neural re-ranker).

# Build and run

The firmware is a freestanding Zephyr application built against nRF Connect SDK **v3.4.1** (Zephyr 4.4.2) with Zephyr SDK **1.0.1**. One-time setup, in a workspace outside this repository:

```
sudo apt install libsdl2-dev                 # emulator only
mkdir -p ~/ncs && cd ~/ncs
python3 -m venv .venv && . .venv/bin/activate && pip install west
west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.4.1 v3.4.1
cd v3.4.1 && west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt -r nrf/scripts/requirements-build.txt
# Zephyr SDK 1.0.1: minimal bundle plus the arm-zephyr-eabi toolchain, unpacked to
# ~/ncs/zephyr-sdk-1.0.1 (toolchain in gnu/), then: ./setup.sh -c
```

Each shell:

```
. ~/ncs/.venv/bin/activate
export ZEPHYR_BASE=~/ncs/v3.4.1/zephyr ZEPHYR_SDK_INSTALL_DIR=~/ncs/zephyr-sdk-1.0.1
```

Build, in `sw/`:

```
west build -b native_sim/native/64 --no-sysbuild -d build-sim .     # emulator
./build-sim/zephyr/zephyr.exe

west build -b cmdz5/nrf52840 --no-sysbuild -d build-board .        # board
west flash -d build-board                                           # SWD (J-Link): application + language models
```

`west flash` writes `merged.hex`: the application and the language model pack for the `lm` partition (built by `modules/lm_image`, a small Zephyr module, because the flash runner is configured before the application's own CMake code runs). Without the models the device still works; text input then offers multitap and 123 only.

On the board the log goes to SEGGER RTT, because the only UART belongs to the GPS. USB CDC ACM follows later.

Fonts are generated with `tools/fontconv/gen_fonts.sh`; the generated headers are committed.

The language models are rebuilt with `tools/lmbuild/build_models.sh` (needs numpy; downloads about 170 MB of corpora once); the built `lm/*.lm` files are committed. Host tests of the text input library: `tests/textinput/run.sh`.

# Development plan

| Phase | Content | Needs hardware |
|---|---|---|
| 1 ✅ | Board definition, `native_sim` front panel (display model, keys, wheel, LEDs, audio), `gfx` + `ui`, home, menu, settings and hardware test screens | no |
| 2 ✅ | Multitap, `lmbuild` pipeline, T9 with n-gram ranking and next-word prediction, user model, evaluation, compose screen | no |
| 3 ✅ | Meshtastic codec, crypto with test vectors, router, nodedb, conversations/chat/nodes screens, mesh settings, UDP radio between emulators | no |
| 4 | Board bring-up: nPM1300 charger and rails, LCD bit-bang driver, keypad, QDEC, LEDs, vibration, PWM audio. Battery measurement, fuel gauge and diagnostics screen are written and run in the emulator | yes |
| 5 | SX1262 on the board, over-the-air test against a stock Meshtastic node | yes |
| 6 | LoRaWAN on TTN, radio arbiter, duty-cycle accounting | yes |
| 7 | GPS, power states, measuring the <20 µA low-power current | yes |

Phases 1–3 can run before the boards arrive.

# Later extensions

* **Meshtastic PhoneAPI** over BLE and USB serial (ToRadio/FromRadio), so the official app can configure the device.
* **External SPI NOR flash** (rev 2, CS on P0.04, shared bus with the SX1262): larger language models, more message history.
* **Neural re-ranker** for predictive text (small int8 GRU or transformer over the top-k n-gram candidates).
* **Microphone:** voice notes are not practical over LoRa, but the microphone could serve as a clap or noise detector.
* **Firmware update** over BLE (MCUboot dual slot needs the external flash).

# Open points

* Update the language models separately from the application (mcumgr image upload to the `lm` partition).

* The display controller is unknown, and its init sequence has not been tested on real hardware. The emulator models only the behaviour documented in `../display/DOCUMENTATION.md`.
* Meshtastic protocol changes between releases: the implementation follows firmware 2.7; check new releases (e.g. next-hop routing, signed packets).
* SX1262 driver for the board with the Meshtastic sync word 0x2b (the Zephyr lora API only offers 0x34/0x12), BUCK1 switching and TCXO; until then the board reports "Radio not available".
* Battery model for the real cell (see [Battery and current measurement](#battery-and-current-measurement-srcbatteryc)); the fuel gauge code is untested on hardware.
* Radio sharing between Meshtastic and the Zephyr LoRaWAN stack (see [Radio arbiter](#radio-arbiter-meshtastic-and-lorawan-on-one-sx1262)).
* Licences: no Meshtastic code is used (protocol re-implemented). The language models are CC BY derived works of their corpora (see `lm/README.md`).
