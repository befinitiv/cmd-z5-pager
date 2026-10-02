# Sony CMD-Z5 Display Interface Protocol

This document describes the interface between the main CPU of a Sony CMD-Z5 mobile phone and its 96 × 72 pixel, 4-level grayscale display. The protocol was reverse-engineered from logic analyzer captures that start at power-up.

The decoded pixel data shows the expected boot animation, the SONY logo and the final "PIN eingeben" screen, so the physical layer, framing and pixel format are confirmed. The meaning of most configuration registers is **not** confirmed: the descriptions in the register table are guesses based only on the values written.

## Connector

The FFC cable that is inserted into the connector has a total width of 13.5mm and a pitch of 0.5mm. The cable connects on the top side.

## Captures

Three captures of the same power-up sequence were made. All three contain the identical sequence of 165,066 display writes. This document is based on **capture 3**, the only one with all probes on the correct pins. The other two were used only to cross-check it.

| Capture | File                                    | Channels | Notes |
|--------:|-----------------------------------------|---------:|-------|
| 1       | `DSLogic U3Pro32-la-260929-090124.dsl`  | 12       | Data bus and `/WR`, no A0. |
| 2       | `DSLogic U3Pro32-la-260929-110147.dsl`  | 13       | Probes misconfigured (D3 missing). Superseded. |
| 3       | `DSLogic U3Pro32-la-260929-111226.dsl`  | 14       | **Reference.** All signal pins, mapped as in `PINS.md`. |

All three were recorded with a DSLogic U3Pro32 (DSView v1.3.0) at **20 MHz** (50 ns resolution) for 400 M samples (20 s), with a 1.4 V threshold.

All times in this document are trace times, measured from the start of the capture. The captures differ from each other by less than 1 ms.

## Physical layer

### Connector pinout

The display connects through a 26-pin connector. The logic supply (VCC) is **2.9 V**. Trace channel numbers refer to capture 3, as listed in `PINS.md`.

| Pin | Trace | Signal | Notes |
|----:|-------|--------|-------|
| 1   | –     | GND    | |
| 2   | –     | GND    | |
| 3   | –     | VCC    | |
| 4   | –     | VCC    | |
| 5   | –     | GND    | |
| 6   | –     | VCC    | |
| 7   | –     | GND    | |
| 8   | –     | VCC    | |
| 9   | T10   | ?      | Low for the whole capture, with no edges at all. Possibly tied low (for example a mode-select pin). |
| 10  | T0    | **`/WR`** | Write strobe, active low. It goes high at 0.184 s and idles high after that. |
| 11  | –     | VCC    | |
| 12  | T1    | **`/RESET`** (probably) | Low from power-up, goes high once at 0.595 s and stays high. The first write follows about 5 ms later. |
| 13  | T11   | **A0** (register select) | Low = register index, high = data. See [Register select](#register-select). |
| 14  | T12   | **`/CS`** | Chip select, active low. See [Chip selects](#chip-selects). |
| 15  | T13   | **`CS`** | Chip select, active high: the inverse of pin 14. |
| 16  | –     | VCC    | |
| 17  | T2    | D7     | Data bus MSB |
| 18  | T3    | D6     | |
| 19  | T4    | D5     | |
| 20  | T5    | D4     | |
| 21  | T6    | D3     | |
| 22  | T7    | D2     | |
| 23  | T8    | D1     | |
| 24  | T9    | D0     | Data bus LSB |
| 25  | –     | VCC    | |
| 26  | –     | GND    | |

The data bus is on pins 17–24 in order, D7 to D0. The bit order was first determined from the register indices, which count up sequentially (`05 06 07 08 09 0A …`) only with this bit order. Capture 3 confirms it: every one of the eight data channels matches the bytes decoded from capture 1 in 100% of the 165,066 writes.

The data bus is shared with other devices. It toggles about 10 million times during the capture, but only the bytes that are on the bus while `/WR` is low are meant for the display.

### Register select

**Pin 13 (T11) is the register-select line (A0):**

| A0 (pin 13) | Byte type      | Writes |
|------------:|----------------|-------:|
| 0           | Register index | 17,106 |
| 1           | Data           | 147,960 |

This matches the framing by register lengths (see [Framing](#framing)) for every single byte, with no exceptions. A0 is constant for the whole `/WR` low phase of every write.

A0 behaves like a CPU address line: it is low in 99.3% of the capture and also changes outside display writes. The display is therefore probably mapped at two CPU addresses, one for index writes and one for data writes.

### Chip selects

Pins 14 (T12) and 15 (T13) are inverses of each other: they differ in 99.9% of samples. During every display write, pin 14 is low and pin 15 is high. This fits an active-low chip select and an active-high chip select, both driven from the same address decoder. Some display controllers have such a pair (often called `/CS1` and `CS2`).

These pins are not exclusive to the display:

- **Other bus cycles:** pin 14 goes low 1,028,529 times, far more than the 165,066 display writes.
- **No display reads:** during the other pulses, `/WR` stays high, A0 is low, and the data bus shows varying values that are often not even stable within the pulse. These are most likely accesses to other devices in the same address region.

Only `/WR` (pin 10) is exclusive to the display: it pulses only for display writes.

No `/RD` line was found among the connector pins, and no read cycles were observed. The display is probably write-only.

### Write cycle timing

These values are from capture 3.

| Parameter                                  | Observed value                         |
|--------------------------------------------|----------------------------------------|
| `/WR` low pulse width                      | 300–350 ns (6–7 samples)               |
| Shortest write cycle (falling to falling)  | about 350 ns                           |
| Data, A0 or chip-select changes while `/WR` is low | none (0 of 165,066 writes)     |
| Data setup time before `/WR` falling       | often none: the data changes in the same 50 ns sample in 9,745 writes (about 6%), and 1 sample (50 ns) before in another 28,697 (about 17%) |
| A0 and `/CS` setup time before `/WR` falling | 0–50 ns in most writes, like the data |
| Setup time before `/WR` rising (all signals) | at least 300 ns (the whole low phase) |
| Data hold time after `/WR` rising          | at least 50 ns in all writes, usually 100–150 ns |
| A0 hold time after `/WR` rising            | less than 50 ns in 52,159 writes (32%): A0 changes in the same sample as `/WR` rises |
| `/CS` (pin 14) hold time after `/WR` rising | less than 50 ns in 51,521 writes (31%) |

### Latch edge

Data is latched on the **rising edge of `/WR`**, as on an Intel 8080-style bus:

- **Falling edge doesn't work.** In about 6% of writes, the data changes in the same 50 ns sample as `/WR` goes low, so there is essentially no setup time. A controller latching on the falling edge would regularly catch the bus mid-change.
- **Rising edge works.** The data, A0 and chip selects are stable for the full 300–350 ns low phase before the rising edge.
- **Very short hold times.** The data stays unchanged for at least 50 ns after the rising edge. A0 and `/CS` often change within the same 50 ns sample, so the controller must tolerate a hold time close to 0 ns for these. This is typical for 8080-style interfaces, where the minimum address and chip-select hold time is often specified as 0 ns.

These values are limited by the 20 MHz sample rate: everything is quantised to 50 ns, and the 1.4 V threshold hides small skew between the lines. A capture at a higher sample rate would narrow them down.

## Framing

Every transfer starts with a one-byte **register index**, written with A0 = 0. It is followed by a fixed number of data bytes, written with A0 = 1, that depends on the register:

| Register      | Data bytes | Content                                    |
|---------------|-----------:|--------------------------------------------|
| `0x0B`        | 24         | Pixel data for one row (96 px × 2 bit)     |
| `0x10`        | 16         | Table (all `00` in this capture)           |
| `0x14`        | 16         | Table (all `00` in this capture)           |
| all others    | 1          | Register value                             |

With these lengths, all 165,066 bytes of the capture parse into complete commands, with no leftover bytes and no unknown registers. The A0 line confirms this framing for every byte.

Whether the controller requires these exact lengths is unknown: with A0 available, it may simply accept any number of data bytes after an index.

### Typical gaps between writes

These gaps come from how the phone's firmware drives the bus; they are not necessarily requirements of the display.

| Situation                                        | Gap (falling to falling) |
|--------------------------------------------------|--------------------------|
| Init: index → value                              | about 2.8 µs             |
| Init: value → next index                         | about 4.5 µs             |
| Row header (`03 00 04 nn 0B`), byte to byte      | 0.75–1.15 µs             |
| After index `0x0B`, before first pixel byte      | about 21 µs              |
| Pixel bytes within a row                         | 350–800 ns               |
| After last pixel byte, before next row           | 15–16.5 µs               |

## Pixel data

### Addressing

Each row is written with this sequence:

```
03 00        ; register 0x03 = 0x00  (column start, always 0 in this capture)
04 <row>     ; register 0x04 = row number, 0x00 … 0x47 (0 … 71)
0B <24 bytes>; pixel data for that row
```

That is 29 bytes per row. The firmware always writes all 72 rows in order, 0 to 71, so a full frame is 72 × 29 = **2088 bytes**. A frame is often split into two bursts because other CPU activity interrupts it.

Whether the row address auto-increments after a `0x0B` write is unknown, because the firmware always sets registers `0x03` and `0x04` explicitly before each row.

### Pixel format

Each byte holds 4 pixels at 2 bits per pixel. The leftmost pixel is in the most significant bits:

```
bit:     7 6 | 5 4 | 3 2 | 1 0
pixel:   x+0 | x+1 | x+2 | x+3
```

Byte *n* (0…23) of a row covers pixels 4n … 4n+3.

| Value | Gray level   |
|------:|--------------|
| 0     | White        |
| 1     | Light gray   |
| 2     | Dark gray    |
| 3     | Black        |

The level mapping follows from the rendered images: the boot animation is mostly `FF` (black), and the static screen is mostly `00` (white) with black text.

## Registers

Values are exactly as captured. The **Guessed meaning** column is speculation.

| Reg  | Values written     | Guessed meaning |
|------|--------------------|-----------------|
| `00` | `04` → `00` → `40` | Display control. `04` might be standby or reset, `00` display off, `40` (bit 6) display on. |
| `01` | `00`               | ? |
| `02` | `01`               | ? (first register written) |
| `03` | `00`               | Column / X start address |
| `04` | `00`…`47`          | Row / Y address |
| `05` | `47`               | `0x47` = 71 = last row; possibly the display height or end row |
| `06` | `27`               | ? (size or timing) |
| `07` | `11`               | ? (timing) |
| `08` | `25`               | ? (timing) |
| `09`–`0A` | `00`          | ? |
| `0B` | 24 bytes           | Pixel data write (one row) |
| `0C`–`0F` | `00`          | ? |
| `10` | 16 × `00`          | Table, possibly gray-scale or gamma |
| `11`–`13` | `00`          | ? |
| `14` | 16 × `00`          | Table, possibly gray-scale or gamma |
| `15` | `00`               | ? |
| `17`–`1A` | `00 10 18 20` | Four increasing values: possibly the drive levels of the 4 gray levels (set A) |
| `1B`–`1E` | `00 14 1A 20` | Four increasing values: possibly the drive levels of the 4 gray levels (set B) |
| `20` | `DF` → `1F`        | Power control? Bit 7 is cleared just before the display is turned on. |
| `21` | `52`               | Power, bias or booster setting? |
| `22` | `65`               | Power, bias or booster setting? |
| `23` | `80` → `7B` → `85` | Contrast / electronic volume? Always written together with `24`. |
| `24` | `80` → `7B` → `85` | Contrast / electronic volume? Always written with the same value as `23`. |
| `25` | `00`               | ? |

Registers `16`, `1F` and above `25` are never written.

## Observed sequence

### 1. Power-up and reset

| Time      | Event |
|-----------|-------|
| 0.184 s   | `/WR` goes high (idle). |
| 0.595 s   | `/RESET` (pin 12) goes high. |

### 2. Initialization (0.59996 s)

```
02=01  00=04  01=00
05=47  06=27  07=11  08=25
09=00  0A=00  0C=00  0D=00  0E=00  0F=00
10=[00 × 16]
11=00  12=00  13=00
14=[00 × 16]
15=00
20=DF  21=52  22=65  23=80  24=80  25=00
00=00
```

Register `0B` is skipped here; it is the pixel write register. The whole block takes about 290 µs.

### 3. First frame (0.6000 s)

A full frame of all white pixels (`00`) is written while the display is still off.

### 4. Display on (0.7934 s)

```
23=7B  17=00  18=10  19=18  1A=20
24=7B  1B=00  1C=14  1D=1A  1E=20
20=1F
00=40
```

Registers `23`/`17`–`1A` and `24`/`1B`–`1E` are written as two parallel groups. That grouping suggests two sets of parameters, for example for alternating frames or for two drive phases.

### 5. Frames and contrast

| Time          | Event |
|---------------|-------|
| 0.8447 s      | Another all-white frame. |
| 0.8698 s      | `23=85  24=85` (contrast adjusted). |
| 0.905–4.57 s  | Boot animation, one full frame about every 55–110 ms, ending with the SONY logo. |
| 4.722 s       | Static screen: "PIN eingeben", battery icon, cursor bar. |
| 5.4–18.9 s    | The identical static screen is redrawn every 1.1–2.3 s. |

In total, 79 full frames are written, of which 35 are unique. After 0.87 s, only pixel data is written; no further configuration registers change.

## Minimal driver sketch

The following sequence reproduces what the phone does. It is untested on real hardware.

```c
// wr(a0, b): set A0 (pin 13) and put b on D7..D0 (pins 17..24), with /CS (pin 14) low and CS (pin 15) high;
//            pulse /WR low for >= 300 ns and keep A0 and b stable until /WR is high again
//            (rising edge latches)
void idx(uint8_t r)  { wr(0, r); }   // A0 = 0: register index
void dat(uint8_t v)  { wr(1, v); }   // A0 = 1: data
void reg(uint8_t r, uint8_t v) { idx(r); dat(v); }

void lcd_init(void) {
    // /RESET low, then high, wait >= 5 ms
    reg(0x02,0x01); reg(0x00,0x04); reg(0x01,0x00);
    reg(0x05,0x47); reg(0x06,0x27); reg(0x07,0x11); reg(0x08,0x25);
    reg(0x09,0x00); reg(0x0A,0x00); reg(0x0C,0x00); reg(0x0D,0x00);
    reg(0x0E,0x00); reg(0x0F,0x00);
    idx(0x10); for (int i = 0; i < 16; i++) dat(0x00);
    reg(0x11,0x00); reg(0x12,0x00); reg(0x13,0x00);
    idx(0x14); for (int i = 0; i < 16; i++) dat(0x00);
    reg(0x15,0x00);
    reg(0x20,0xDF); reg(0x21,0x52); reg(0x22,0x65);
    reg(0x23,0x80); reg(0x24,0x80); reg(0x25,0x00);
    reg(0x00,0x00);                       // display off

    lcd_frame(white_frame);               // clear before switching on

    reg(0x23,0x7B); reg(0x17,0x00); reg(0x18,0x10); reg(0x19,0x18); reg(0x1A,0x20);
    reg(0x24,0x7B); reg(0x1B,0x00); reg(0x1C,0x14); reg(0x1D,0x1A); reg(0x1E,0x20);
    reg(0x20,0x1F);
    reg(0x00,0x40);                       // display on

    reg(0x23,0x85); reg(0x24,0x85);       // contrast
}

// fb: 72 rows x 24 bytes, 2 bpp, leftmost pixel in bits 7..6, 0 = white, 3 = black
void lcd_frame(const uint8_t fb[72][24]) {
    for (int y = 0; y < 72; y++) {
        reg(0x03, 0x00);
        reg(0x04, y);
        idx(0x0B);
        for (int x = 0; x < 24; x++) dat(fb[y][x]);
    }
}
```

## Decoder

`decode_cmdz5.py` in this folder reads a `.dsl` capture directly, prints every non-pixel register write with its timestamp, and saves each unique full frame as a PNG (scaled 4×):

```
python3 decode_cmdz5.py "DSLogic U3Pro32-la-260929-111226.dsl" frames
python3 decode_cmdz5.py "DSLogic U3Pro32-la-260929-090124.dsl" frames --no-a0
```

- **Capture 3 (default):** the decoder splits the byte stream using A0 and notes any register whose data length differs from the expected one.
- **Capture 1 (`--no-a0`):** this capture has no A0 channel, so the decoder splits the stream by the known register lengths instead.

Both give an identical register log and 35 pixel-identical frames. The decoder needs `numpy` and `Pillow` and takes about 10 s.

## Open questions

- **Controller chip:** unknown. Its datasheet would confirm or correct the register meanings above.
- **Pin 9:** always low; its function is unknown.
- **Pins 14 and 15:** very likely an active-low and an active-high chip select, but they are shared with other devices on the bus.
- **Row auto-increment:** unknown, because the firmware always sets the address before each row.
- **Partial writes:** unknown whether register `0x03` can set a column offset other than 0, and whether shorter `0x0B` bursts are accepted.
- **Reads:** no read cycles were captured and no connector pin behaves like `/RD`, so the display is probably write-only.
