This document describes a PCB that acts as a replacement PCB for a Sony CMD-Z5 mobile phone. It is supposed to use the original screen and keyboard and turn the phone into a meshtastic device.

# Requirements

* nrf52840 CPU
* WIO-SX1262 wireless module with PCB antenna
* TBS M10N GPS with switched power line for power saving
* Battery charging and monitoring
* USB-C connector for charging and data/programming
* Use all keys of the original phone
* Use vibration motor, earpiece speaker and loudspeaker of original phone
* Use LEDs of original phone
* Uses small components so that PCB fits inside of housing
* Use 0402 passive components whenever possible
* Acceleromenter for movement detection
* Optimize for quiescent current. The device is also supposed to run in a low power mode where the idle current is expected to be below 20uA. This might require power switches to certain components.

# Component selection

LCSC part numbers were checked on 2026-10-02. Check stock again before ordering.

| Function | Part | LCSC | Package | Notes |
|---|---|---|---|---|
| MCU | Nordic nRF52840-QIAA-R | C190794 | aQFN-73 7×7 | Needs machine assembly |
| 32 MHz crystal | Yangxing X1E0000210163 | C91742 | 3225 | 10 pF, ±10 ppm |
| 32.768 kHz crystal | Epson X1A0000610006 | C99009 | 2012 | 7 pF |
| BLE antenna | Johanson 2450AT18A100E | C89334 | 1206 | Chip antenna with pi matching network |
| LoRa | Seeed Wio-SX1262 | – | 11.6×11 mm, 12 pins | **Not available on LCSC**; buy from Seeed. The standard version uses its IPEX connector and leaves the ANT pad (pin 9) unconnected; a PCB antenna needs the RF SMT-pin version (ask Seeed sales) |
| PMIC (charger, 2 bucks, 2 load switches, ADC, USB-C detection) | Nordic nPM1300-QEAA-R7 | C7501206 | QFN-32 5×5 | 0.8 µA battery current with a buck enabled, 370 nA in ship mode |
| GPS supply | TI TPS610997YFFR | C2072359 | DSBGA-6 | Fixed 5 V boost; EN fully disconnects the GPS (TBS M10N needs 3.6–5.5 V) |
| Accelerometer | ST LIS2DH12TR | C110926 | LGA-12 2×2 | About 2 µA in low-power motion detection; same WHO_AM_I as the LIS3DH. Alternative: LIS2DW12TR (C189624), lower current but needs its own driver |
| Earpiece amplifier | Diodes PAM8302AASCR | C113367 | MSOP-8 | BTL output for the floating earpiece; input from nRF PWM via RC filter |
| LED and loudspeaker drivers | Diodes DMG1024UV-7 | C2759893 | SOT-563 | Dual N-FET, low-side switches |
| Vibration motor driver | Diodes DMG1016UDW-7 | C211433 | SOT-363 | N+P pair, high-side switch (motor's other terminal is tied to GND) |
| Flyback diodes | 1N5819WS | C191023 | SOD-323 | For motor and loudspeaker |
| Power button diodes | BAT54C | C916424 | SOT-23 | Diode-OR of the power button to nPM1300 SHPHLD and an nRF GPIO |
| USB-C connector | GCT USB4105-GF-A | C3020560 | SMD | |
| USB ESD protection | ST USBLC6-2SC6 | C7519 | SOT-23-6 | |
| Display connector | Kinghelm KH-FPC0.5-H1.0SMT-26P-QCHF | C3250122 | SMD | 26-pin, 0.5 mm, 1.0 mm high, flip lock, double-sided contacts (display flex is top contact) |
| Keyboard flex connector | Kinghelm KH-FPC0.5-H1.0SMT-28P-QCHF | C3250123 | SMD | 28-pin, 0.5 mm, 1.0 mm high, double-sided contacts (keyboard flex is bottom contact) |

The TCA8418 keypad controller was considered and dropped: it draws 3 µA in standby. The nRF52840 scans the matrix itself.

# Power architecture

```
USB-C ─► nPM1300 ─ VSYS ─┬─ BUCK2 3.0 V (150 kΩ on VSET2, on at boot) ─► nRF52840, LIS2DH12
                         │     ├─ LDSW1 ─► display VCC          (off in low-power mode)
                         │     └─ LDSW2 ─► earpiece amplifier   (off in low-power mode)
                         ├─ BUCK1 3.0 V (VSET1 grounded = off at boot) ─► Wio-SX1262 (enabled by firmware over I2C)
                         ├─ TPS610997 5 V (EN from nRF) ─► TBS M10N
                         └─ LED/loudspeaker common, vibration motor
```

* All logic runs from a single 3.0 V rail. The display needs 2.9 V, so 3.0 V needs no level shifters. The cost is roughly 0.5–1 dB less maximum TX power than at 3.3 V.
* BUCK2 is the always-on rail because a VSET resistor can set BUCK1 to at most 2.7 V at boot.
* Each nPM1300 buck supplies up to 200 mA. The SX1262 (about 118 mA in TX) has BUCK1 to itself.
* The nPM1300 starts with charging disabled. Firmware must configure the charge current, termination voltage and NTC over I2C. Battery voltage is read from the nPM1300 ADC, so there is no resistor divider.
* The nPM1300 CC1/CC2 pins connect directly to the USB-C connector. They have internal pull-downs, so no 5.1 kΩ resistors are needed.

# Low-power mode

Low-power mode means the LoRa radio is off; an SX1262 in RX draws about 5 mA. The device wakes on a key press, the lid, the power button, accelerometer motion or the RTC.

| Part | Current |
|---|---|
| nPM1300 (battery current, buck enabled) | 0.8 µA |
| nRF52840 asleep, RTC running, RAM retained | ~2–3 µA |
| LIS2DH12 motion detection | ~2 µA |
| SX1262, display, earpiece amplifier, GPS | ~0 (switched off) |
| Leakage (FETs, ESD diode, boost in shutdown) | < 1 µA |
| **Total** | **about 6–7 µA** |

Design rules to stay within this budget:

* **Lid switch:** it is probably closed while the phone is folded shut. With the nRF internal pull-up (about 13 kΩ) that is about 230 µA. Use a 4.7–10 MΩ external pull-up and disable the internal one. The same applies to the rotary encoder if it can rest with a contact closed.
* **Back-powering:** when the display, SX1262 or GPS is switched off, drive their signal lines low or set them as floating inputs.
* **Keypad wake:** in sleep, drive all matrix columns low and use GPIO SENSE on the rows, so any key wakes the nRF with no static current.
* **Power button:** connect it to nPM1300 SHPHLD (wakes from ship mode) and to an nRF GPIO through the BAT54C. SHPHLD has a 50 kΩ pull-up to VBAT, so a direct connection would put battery voltage on a 3.0 V GPIO.

# Design notes

* **Display bus:** the nRF52840 has no parallel bus peripheral, so the display is bit-banged. Put D0–D7 on 8 consecutive P0.x pins so one register write sets the byte. Tie `/CS` (pin 14) low, CS (pin 15) high and pin 9 low, leaving 11 signals: D0–D7, `/WR`, A0 and `/RESET`. P0.09/P0.10 need NFC disabled in the UICR if used.
* **Rotary encoder:** use the nRF quadrature decoder (QDEC).
* **GPS UART:** the M10N probably has 3.3 V I/O; put about 1 kΩ in series on its TX line into the nRF.
* **GPIO budget:** about 43 of the roughly 45 usable nRF52840 GPIOs are needed (48 minus the 32 kHz crystal and reset pins).
* **0402 passives:** use 0402 everywhere except the buck, boost and nRF DC/DC inductors (0603/0805 for current rating) and possibly a 22 µF bulk capacitor on VSYS (0603).
* **RTC battery (keyboard flex pin 23):** the nRF has no battery-backed domain. Leave it unconnected or route it to an ADC pin.
* **Battery:** assumed to be a 1S Li-ion/LiPo cell fitted in the original battery compartment.

# nRF52840 GPIO assignment

Package aQFN73. Pins marked "low frequency I/O only" in the nRF52840 product specification (signals up to 10 kHz) are used only for slow signals: keypad, buttons, encoder, interrupts and enables. Fast signals use the other pins: display bus, SPI, I2C, UART and audio PWM.

| Ball | GPIO | Side | Low freq only | Signal | Direction | Notes |
|---|---|---|---|---|---|---|
| D2 | P0.00 | top | | XL1 | – | 32.768 kHz crystal |
| F2 | P0.01 | top | | XL2 | – | 32.768 kHz crystal |
| G1 | P0.26 | top | | Display D7 | out | |
| H2 | P0.27 | top | | Loudspeaker PWM | out | Gate of DMG1024UV channel 2 |
| J1 | P0.04 | top | | *spare* (AIN2) | | |
| K2 | P0.05 | top | | *spare* (AIN3) | | Optional: RTC battery sense (keyboard flex pin 23) |
| L1 | P0.06 | top | | Earpiece PWM | out | RC filter to PAM8302 input |
| M2 | P0.07 | top | | Vibration motor | out | Gate of DMG1016UDW N-FET |
| N1 | P0.08 | top | | LED control | out | Gate of DMG1024UV channel 1 |
| P2 | P1.08 | top | | I2C SDA | i/o | nPM1300, LIS2DH12 |
| R1 | P1.09 | top | | I2C SCL | out | nPM1300, LIS2DH12 |
| T2 | P0.11 | top | | GPS UART RX | in | From M10N TX, ~1 kΩ series |
| U1 | P0.12 | top | | GPS UART TX | out | To M10N RX |
| AD8 | P0.13 | right | | SX1262 SCK | out | |
| AC9 | P0.14 | right | | SX1262 MOSI | out | |
| AD10 | P0.15 | right | | SX1262 MISO | in | |
| AC11 | P0.16 | right | | SX1262 NSS | out | |
| AD12 | P0.17 | right | | Display A0 | out | Display pin 13 |
| AC13 | P0.18 | right | | nRESET | in | Pin reset, SWD connector |
| AC15 | P0.19 | right | | Display D0 | out | Display pin 24 |
| AD16 | P0.20 | right | | Display D1 | out | Display pin 23 |
| AC17 | P0.21 | right | | Display D2 | out | Display pin 22 |
| AD18 | P0.22 | right | | Display D3 | out | Display pin 21 |
| AC19 | P0.23 | right | | Display D4 | out | Display pin 20 |
| AD20 | P0.24 | right | | Display D5 | out | Display pin 19 |
| AC21 | P0.25 | right | | Display D6 | out | Display pin 18 (D7 = P0.26, display pin 17) |
| AD22 | P1.00 | right | | Display /WR | out | Display pin 10 |
| L24 | P0.09 | bottom | yes | SX1262 RF switch | out | NFC pin; Wio-SX1262 pin 1 `RF_SW`, driven as `SX126X_RXEN` in Meshtastic |
| J24 | P0.10 | bottom | yes | SX1262 NRESET | out | NFC pin |
| Y23 | P1.01 | bottom | yes | SX1262 BUSY | in | |
| W24 | P1.02 | bottom | yes | SX1262 DIO1 | in | IRQ |
| V23 | P1.03 | bottom | yes | GPS power enable | out | TPS610997 EN |
| U24 | P1.04 | bottom | yes | Accelerometer INT1 | in | Wake on motion |
| T23 | P1.05 | bottom | yes | nPM1300 interrupt | in | From an nPM1300 GPIO |
| R24 | P1.06 | bottom | yes | Power button | in | Via BAT54C (shared with nPM1300 SHPHLD) |
| P23 | P1.07 | bottom | yes | Display /RESET | out | Display pin 12 |
| A8 | P0.31 | left | yes | Keypad COL0 | out | Flex pin 2 |
| B9 | P0.30 | left | yes | Keypad COL1 | out | Flex pin 3 |
| A10 | P0.29 | left | yes | Keypad COL2 | out | Flex pin 4 |
| B11 | P0.28 | left | yes | Keypad COL3 | out | Flex pin 5 |
| A12 | P0.02 | left | yes | Keypad ROW0 | in | Flex pin 6, pull-up + SENSE |
| B13 | P0.03 | left | yes | Keypad ROW1 | in | Flex pin 7, pull-up + SENSE |
| A14 | P1.15 | left | yes | Keypad ROW2 | in | Flex pin 8, pull-up + SENSE |
| B15 | P1.14 | left | yes | Keypad ROW3 | in | Flex pin 9, pull-up + SENSE |
| A16 | P1.13 | left | yes | Keypad ROW4 | in | Flex pin 22, pull-up + SENSE |
| B17 | P1.12 | left | yes | Rotary encoder A | in | Flex pin 20, QDEC |
| B19 | P1.11 | left | yes | Rotary encoder B | in | Flex pin 21, QDEC |
| A20 | P1.10 | left | yes | Lid switch | in | Flex pin 1, 4.7–10 MΩ external pull-up |

That uses 43 GPIOs and leaves 2 spare (P0.04, P0.05). SWDIO, SWDCLK, D+, D− and VBUS are dedicated pins and need no GPIO.

Display pins tied on the board, not connected to GPIOs: `/CS` (pin 14) to GND, `CS` (pin 15) to the 3.0 V display supply, pin 9 to GND.

Notes:

* **Display data bus:** D0–D7 are on consecutive pins P0.19–P0.26, so a byte is written as `(b << 19)` through `OUTSET`/`OUTCLR` (or a masked `OUT` write). D0–D6 and the control lines are on the right side of the package; D7 (P0.26) is on the top side.
* **NFC pins:** P0.09/P0.10 must be configured as GPIOs in UICR (`NFCPINS`). If the two pins are driven to different levels, a small current flows between them. Both carry SX1262 outputs that are driven low while the radio is off, so they are at the same level in low-power mode.
* **Trace pins:** P0.07, P0.11, P0.12, P1.00 and P1.09 are used as GPIOs, so ETM trace and SWO are not available. SWD debugging works.
* **Wake from sleep:** keypad rows, rotary encoder, lid, power button, accelerometer INT1 and nPM1300 interrupt can all use GPIO SENSE, including in System OFF.

# Schematic

The KiCad 10 project is in `kicad/` (`cmd-z5.kicad_pro`). The root sheet has four sub-sheets: Power (USB-C, nPM1300, battery), MCU (nRF52840, crystals, BLE antenna, SWD, LIS2DH12), Radio_GPS (Wio-SX1262, TPS610997 + TBS M10N) and UI (display and keyboard FFCs, earpiece amplifier, LED/loudspeaker/vibration drivers, power key). Sheets are connected by global labels and power symbols. Custom symbols (nPM1300, Wio-SX1262, LIS2DH12, DMG1016UDW, TPS610997 DSBGA, project power symbols) are in `kicad/cmd-z5.kicad_sym`.

The schematic was generated by `kicad/generator/design.py` (`cd kicad/generator && python3 design.py ..`). Re-running it overwrites the sheets, so stop using it once the schematic is edited by hand.

Open points before layout:

* ERC reports nRF52840 DCCH (power output) connected to +3V0. This follows Nordic reference circuit configuration 5 (DCCH tied to VDD) and can be excluded.
* Custom footprints are in `kicad/cmd-z5.pretty` with 3D models in `kicad/cmd-z5.3dshapes`: Wio-SX1262 (drawn from the Seeed datasheet) and the two FFC connectors J5 (C3250122) and J6 (C3250123), imported from LCSC/EasyEDA with easyeda2kicad. The EasyEDA STEP model of C3250122 has a wrong origin, so J5 uses its WRL model.
* Values to tune on the prototype: crystal load capacitors, BLE and LoRa antenna matching, LED and loudspeaker series resistors.
* Check the TBS M10N cable pin order against the 4-pin connector.
