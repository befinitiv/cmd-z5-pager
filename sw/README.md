This document proposes the software architecture for the CMD-Z5 replacement board described in `../board/README.md`. The firmware turns the phone into a standalone Meshtastic and LoRaWAN (TTN) messenger with its own UI and predictive text input. The same code also builds for the PC, where an emulator window stands in for the phone hardware.

Status: phase 1 is implemented (board definition, emulator, graphics, UI toolkit, menus and hardware test). See [Build and run](#build-and-run) and [Development plan](#development-plan).

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
│   ├── lora_emul.c                  # (later) lora API stub, optionally a UDP "ether" between instances
│   ├── gnss_emul.c                  # (later) fixed position or NMEA file replay
│   └── pmic_emul.c                  # (later) battery level and USB state from the front panel
├── lib/
│   ├── gfx/                         # 2bpp framebuffer, primitives, fonts, icons, dirty-row tracking
│   ├── ui/                          # screen stack, events, menu, slider, text view
│   ├── textinput/                   # (later) input methods, LM reader, candidate ranking, user model
│   └── meshtastic/                  # (later) nanopb protos, channel crypto, PKI, packet codec, router, nodedb
├── src/
│   ├── main.c                       # UI thread and event loop
│   ├── input.c                      # Zephyr input events → UI events, long press
│   ├── hw.c                         # display, backlight, vibration, tones and melodies
│   ├── power.c                      # backlight and display timeouts, lid, power key
│   ├── i18n.c                       # menu language (English, German)
│   ├── clock.c  config.c  statusbar.c
│   ├── services/                    # (later) radio_arbiter, mesh_svc, lorawan_svc, gps_svc, notify_svc, msgstore
│   └── screens/                     # home, menu, settings, hardware test, power off; (later) inbox, chat, compose, nodes
├── lm/                              # (later) built language models (de.lm, en.lm)
├── tools/
│   ├── fontconv/                    # BDF → C font headers (X11 misc-misc fonts, public domain)
│   ├── lmbuild/                     # (later) corpus → n-gram → prune/quantise → .lm, plus evaluation
│   └── lcd_replay/                  # (later) replays ../display .dsl captures through lcd_ctrl_model
└── tests/emul/smoke.txt             # emulator smoke test script; (later) ztest and host unit tests
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
| PMIC nPM1300 | `regulator`, `charger`, `npm1300` GPIO, fuel gauge library | battery level, charging and USB-present controls on the front panel |
| Accelerometer | `sensor` API (`st,lis2dh`), motion trigger | key `A` sends a motion event |
| Microphone | not used in the first version | – |

Three custom APIs are added because Zephyr has nothing suitable (Zephyr's `display` API has no 2 bpp gray format):

* **`lcd`:** `write_rows(fb, first, count)`, `set_contrast()`, `set_power()`. The framebuffer is in the controller's native format, so rows go out without conversion. The application only sends rows that changed.
* **`lcd_bus`:** `write(a0, buf, len)`, `reset()`, `enable()`. The controller driver (`cmdz5_lcd`) sits above it and is shared by target and emulator. The emulator therefore runs the real init sequence and row-addressing code against a model of the controller, which logs a warning for anything the reverse-engineered documentation does not cover. A bug in the driver shows up on the PC before the board exists. `tools/lcd_replay` (later) will feed the logic analyzer captures through the same model.
* **`audio_out`:** `tone(freq, ms, volume)`, `stop()`, `busy()`, one device per output (`speaker`, `earpiece`). PCM playback through PWM EasyDMA follows later. A melody player sits above it in `src/hw.c`; an RTTTL sequencer for ringtones and notifications follows in `notify_svc`. Meshtastic uses RTTTL for its external notification module.

## Emulator details

`native_sim` builds the firmware as a Linux executable. SDL calls must run on the host side of the native simulator, so the front panel is split into a Zephyr-side part (`frontpanel.c`) and a host-side part (`frontpanel_bottom.c`, built against SDL2). This is the same split that Zephyr's own `display_sdl` uses. The SDL event loop is Zephyr's (`boards/native/common/sdl`); the front panel hooks in with `SDL_AddEventWatch`.

Front panel layout: the display at the top (scaled 5×, with the gray levels mixed into the panel colour, which turns green with the backlight), the keypad below it as clickable buttons whose labels glow green with the keypad LEDs, and on the right the wheel, LED/VIB/SPK/EAR indicators, lid switch and power key. Key mapping:

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
| `Esc` | power key: short = display on/off, hold 1.5 s = switch off; while off, hold 1 s = switch on (restarts the emulator) |
| `F12` | save the display as `cmdz5-screen-NNN.bmp` |

The lid covers only the keypad; the display stays visible. Closing it returns to the home screen and switches the backlight off. Keypad keys are ignored while it is closed, the side controls (wheel, push, bend, power) still work.

Settings are stored in `flash.bin` in the working directory (Zephyr's flash simulator).

After a restart (switching on again) the emulator starts with the same arguments, so a `--script` runs again.

`--script=<file>` runs front panel commands (`wait`, `key`, `press`, `release`, `wheel`, `lid`, `shot`, `quit`, see `emul/script.c`) for UI tests and screenshots. With `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy` it runs without a window and sound, e.g. in CI. `tests/emul/smoke.txt` walks through all current screens.

Planned options: `--node-id`, `--gps lat,lon` and `--ether udp://239.0.0.1:4403`. With the UDP ether, two emulator instances can exchange real Meshtastic packets with each other. This is optional; a plain stub is enough to start.

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

## Meshtastic (`lib/meshtastic`)

| Module | Content |
|---|---|
| `proto/` | nanopb code for a pinned subset of meshtastic/protobufs: `mesh`, `portnums`, `channel`, `config`, `telemetry`. Options files keep string and array sizes small |
| `packet` | 16-byte over-the-air header (dest, from, id, flags with hop limit and hop start, channel hash, next hop, relay node) |
| `crypto` | channel encryption AES-CTR with the PSK (default key expansion, nonce from packet ID and sender), PKI direct messages with X25519 + AES-CCM. Uses the nRF52840 CC310 through PSA crypto, and mbedTLS on the PC |
| `router` | managed flooding: duplicate cache, SNR-weighted rebroadcast delay, hop limit, implicit ACKs, retries for `want_ack`, next-hop routing for DMs |
| `nodedb` | known nodes (name, keys, position, SNR, last heard) with LRU eviction, stored in LittleFS |
| `modules` | TEXT_MESSAGE, NODEINFO, POSITION, TELEMETRY (device metrics), ROUTING (ACK/NAK), TRACEROUTE |
| `channels` | channel list with PSK, frequency slot from the hash of the primary channel name, modem presets (LongFast etc.) |

Compatibility is tested with test vectors taken from the Meshtastic firmware (encrypted packets, channel hashes, frequency slots) and later over the air against a stock Meshtastic node.

## LoRaWAN (`lorawan_svc`)

Uses the Zephyr LoRaWAN subsystem (OTAA, EU868, class A). DevEUI, JoinEUI and AppKey are entered in the settings or over the USB shell and stored with the settings subsystem. The DevNonce is kept persistently because LoRaWAN 1.0.4 requires it. Uplinks: Cayenne LPP for position and battery (so TTN decodes them without a custom formatter), and port 10 for UTF-8 text. Downlinks appear in the inbox as a "TTN" conversation.

## Power (`power_svc`)

| State | Display | Backlight | LoRa | GPS | Typical entry |
|---|---|---|---|---|---|
| Active | on | on | RX | per setting | key press, lid opened |
| Dim | on | off | RX | per setting | 10 s without input, lid closed (also returns to the home screen) |
| Screen off | off (LDSW1 off) | off | RX | per setting | power key, 60 s without input |
| Low power | off | off | off (BUCK1 off) | off | user choice, or low battery |
| Off (ship mode) | – | – | – | – | power key held 1.5 s and released; critical battery (later). Hold the power key about 1 s to switch on again (`ship-to-active-time-ms`). With USB connected the PMIC stays awake and the firmware restarts instead |

`power_svc` enforces the rules from the board README: drive or float the signal lines of switched-off parts, rows on SENSE, columns low. Waking in low-power mode comes from keys, lid, power key, accelerometer or RTC (for scheduled LoRaWAN uplinks). Zephyr device runtime PM switches off the drivers; the nPM1300 regulators are switched through the `regulator` API. Charger setup (current, termination voltage, NTC disabled) happens early in boot, because the nPM1300 starts with charging disabled.

## Storage

| Data | Mechanism | Location |
|---|---|---|
| Configuration (node name, channels, LoRaWAN keys, UI options) | Zephyr settings on ZMS or NVS | `storage` partition |
| Messages, node DB, user dictionary | LittleFS | `lfs` partition |
| Language models | read-only blob, memory-mapped | `lm` partition (later on external flash) |

# User interface

## Display and graphics (`lib/gfx`)

The framebuffer is 96 × 72 pixels at 2 bpp = 1728 bytes, in the native format of the display (24 bytes per row, MSB = leftmost pixel). `gfx` keeps a dirty bit per row, and `cmdz5_lcd` only sends changed rows (29 bytes each). Fonts: a proportional 5×7 font (about 18 characters per line) for text, a 3×5 font for the status bar, a large digit font for the clock, all with German characters. The 4 gray levels are used for anti-aliasing, selection bars and disabled items.

LVGL was considered and rejected: it costs 100+ KB of flash and does not fit this display.

## Layout

```
┌────────────────────────────┐  y 0–6    status bar: battery, mesh/LoRaWAN state, GPS, unread, time
│▮▮▯ M L ⌖ ✉2         12:34 │
├────────────────────────────┤
│ Anna: Bin gleich da.       │  y 8–55   content (6 lines at 8 px)
│ Ich: Super, bis gleich!    │
│ …                          │
├────────────────────────────┤
│ Hallo wie geht│      [T9de]│  compose line with cursor and input mode
│ «es» dir Ihnen euch        │  y 64–71  candidate bar (wheel scrolls, push accepts)
└────────────────────────────┘
```

## Toolkit (`lib/ui`)

A small retained-mode toolkit: a screen stack with `on_enter`, `on_event`, `render` and `on_exit`, and widgets for list/menu, text view with wheel scrolling, text field, candidate bar, number/hex entry, toggle and progress bar. Screens subscribe to zbus channels and redraw only when data changes.

## Key roles

| Key | Navigation | Text entry |
|---|---|---|
| Wheel rotate | move selection, scroll | move through candidates; scroll text when the bar is empty |
| Wheel push | select / open | accept candidate |
| Wheel bend up | back (long: home screen), same as C | back; the draft is kept |
| Wheel bend down | page down | still open |
| CALL | send / confirm | send message |
| C | back (long: home screen) | delete (short), clear word (long), back when the field is empty |
| Memo | context menu | symbols and smileys menu |
| `#` | – | input mode: T9 → Abc multitap → 123 (long press: language) |
| `*` | – | next candidate (classic T9), long press: case Abc / abc / ABC |
| `0` | – | space and accept (long press: 0) |
| `1` | – | punctuation `. , ? ! ' - :` with sentence-aware ranking |

## Menu language

The menus are in English or German (Settings → Language, stored with the other settings). Strings stay English in the code and are translated when drawn: `tr("Settings")` in the application, and the UI library passes titles, menu labels and its own texts through the same hook (`ui_set_translator()`). The German table is in `src/i18n.c`; a missing entry falls back to English. Texts must stay short: a menu row holds about 18 characters.

## Screens

Home (clock, battery, unread count, last message) · Inbox (channels, DMs, TTN) · Chat (scrollback with delivery state: queued / sent / acked) · Compose · Nodes (list sorted by last heard, detail with distance and bearing from GPS, SNR, hops; start a DM or traceroute) · GPS status · LoRaWAN (join state, last uplink, send now) · Settings (Mesh: name, region, preset, channels and PSK · LoRaWAN: keys, interval · Display: contrast, backlight timeout · Sound: ringtone, vibration, earpiece/speaker · Input: languages, learn words on/off, reset dictionary · Power · About).

# Predictive text (`lib/textinput`)

## Input methods

* **Multitap (Abc):** classic letter cycling with timeout, including `ä ö ü ß` and accents on the matching keys.
* **Predictive (T9):** one key press per letter; the engine finds every word whose key sequence matches. German letters map to their base key (ä→2, ö→6, ü→8, ß→7).
* **Numeric (123)** and **symbol picker** (Memo key, chosen with the wheel).

## Engine

For each key press the engine produces a ranked candidate list:

```
key sequence ─► lexicon lookup ─► candidates ─► score with context ─► top N ─► candidate bar
 "488"           exact matches      gut, Hut, hut   P(w | w-2, w-1)
                 + completions       guten, Hütte…   + user model
                                                     + conversation cache
```

1. **Lexicon lookup:** a trie over key sequences (digits), with child sets and word lists stored succinctly in flash. It returns the words with exactly this key sequence and, for completion, the most likely words that start with it.
2. **Context scoring:** an n-gram language model (word trigrams with backoff) gives `log P(w | w-2, w-1)`. This is where the previous words come in: the key sequence `488` matches "gut", "Hut" and "hut". After "Mir geht es" the engine offers "gut" first, and after "Ich trage einen" it offers "Hut", instead of always ranking by overall word frequency.
3. **Next-word prediction:** after a space and before the next key, the candidate bar shows the most likely next words for the context (e.g. "bis" → "gleich", "morgen", "später"). Accepting one costs a single wheel press.
4. **User model:** unigram and bigram counts of words the user sends, with exponential decay, stored in LittleFS. New words typed in multitap are added to a user lexicon and are then available in T9.
5. **Conversation cache:** words from the message being replied to get a short-term boost. Names, places and unusual words from the conversation then rank high.
6. **Interpolation:** `score = λ1·log P_LM + λ2·log P_user + λ3·log P_cache`. The weights are tuned offline on held-out chat text.

Capitalisation: the lexicon keeps the usual case of each word (German nouns), and the engine adds sentence-start capitalisation and the `*` case modes. The scorer is behind a small interface (`ti_scorer`), so a neural re-ranker can be added later without touching the input methods.

## Model format and size

The model is built offline per language, by `tools/lmbuild`:

1. Corpus: conversational text is best for a messenger, e.g. OpenSubtitles from OPUS and the Leipzig Corpora web/news sets. Check the licences before redistributing models.
2. Normalise and tokenise, keep the vocabulary of the 25–30k most frequent words.
3. Train a modified Kneser-Ney trigram model (KenLM `lmplz`), apply entropy pruning to the size budget.
4. Quantise log-probabilities to 8 bits (unigrams) and 4–6 bits (bigrams/trigrams), sort word IDs by frequency, delta-code the successor lists, and write a `.lm` file with a header (language, version, sizes, CRC).
5. Evaluate on held-out text: keystrokes per character, top-1 and top-3 accuracy, and the share of words typed with next-word prediction. The PC emulator and the Python extension use the same C code as the firmware, so the numbers are what the device delivers.

| Part (per language) | Target size |
|---|---|
| Lexicon trie (about 28k words, with case) | 70–90 KB |
| Unigrams | about 28 KB |
| Bigrams (about 60k after pruning) | 70–90 KB |
| Trigrams (frequent contexts only) | 0–30 KB |
| **Total** | **about 170–200 KB** |

## Flash budget (1 MB internal)

| Partition / part | Size |
|---|---|
| MCUboot (single slot, USB serial recovery) | 48 KB |
| Application: Zephyr, drivers, LoRaWAN, Meshtastic + crypto, UI, fonts, text engine | 380–450 KB |
| Settings + LittleFS | 128 KB |
| Language models, German + English | about 360 KB |
| Reserve (BLE stack for the PhoneAPI needs about 120 KB) | rest |

The budget is tight. `lmbuild` takes the partition size as a target and prunes until the model fits. The `lm` partition can be updated separately from the application over USB (mcumgr). With an external SPI NOR flash in rev 2, the models move there and can be 5–10 times larger (bigger vocabulary, full trigrams, optionally a small neural re-ranker).

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
west flash -d build-board                                           # SWD (J-Link)
```

On the board the log goes to SEGGER RTT, because the only UART belongs to the GPS. USB CDC ACM follows later.

Fonts are generated with `tools/fontconv/gen_fonts.sh`; the generated headers are committed.

# Development plan

| Phase | Content | Needs hardware |
|---|---|---|
| 1 ✅ | Board definition, `native_sim` front panel (display model, keys, wheel, LEDs, audio), `gfx` + `ui`, home, menu, settings and hardware test screens | no |
| 2 | Multitap, `lmbuild` pipeline, T9 with n-gram ranking and next-word prediction, user model, evaluation | no |
| 3 | Meshtastic codec, crypto with test vectors, router, nodedb, inbox/chat/compose screens, UDP ether between two emulators | no |
| 4 | Board bring-up: nPM1300 charger and rails, LCD bit-bang driver, keypad, QDEC, LEDs, vibration, PWM audio | yes |
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

* The display controller is unknown, and its init sequence has not been tested on real hardware. The emulator models only the behaviour documented in `../display/DOCUMENTATION.md`.
* Meshtastic protocol changes between releases: pin a protobuf release and update deliberately.
* Radio sharing between Meshtastic and the Zephyr LoRaWAN stack (see [Radio arbiter](#radio-arbiter-meshtastic-and-lorawan-on-one-sx1262)).
* Licences: meshtastic/protobufs is GPL-3.0, which affects the licence of this firmware. Corpus licences affect whether the language models can be published.
