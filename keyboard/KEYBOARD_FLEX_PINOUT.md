# Keyboard Flex PCB Pinout

The flex PCB harness carrying the keyboard, earpiece, loudspeaker and vibration
motor connects to the mainboard via a 28-pin connector.

The flex cable that is inserted into the connector has a total width of 14.5mm and a pitch of 0.5mm. It connects on the bottom side.

## Connector pin assignment

| Pin | Signal                     | Notes                                       |
|----:|----------------------------|---------------------------------------------|
|   1 | Lid button                 | Switch closes to GND                        |
|   2 | COL0                       | Keyboard matrix column 0                    |
|   3 | COL1                       | Keyboard matrix column 1                    |
|   4 | COL2                       | Keyboard matrix column 2                    |
|   5 | COL3                       | Keyboard matrix column 3                    |
|   6 | ROW0                       | Keyboard matrix row 0                       |
|   7 | ROW1                       | Keyboard matrix row 1                       |
|   8 | ROW2                       | Keyboard matrix row 2                       |
|   9 | ROW3                       | Keyboard matrix row 3, via 100 Ω resistor   |
|  10 | Power button, pin 1        |                                             |
|  11 | Power button, pin 2        |                                             |
|  12 | LED control                |                                             |
|  13 | LED / loudspeaker common   |                                             |
|  14 | LED / loudspeaker common   | Connected to pin 13                         |
|  15 | Vibration motor            | Other motor terminal connects to GND        |
|  16 | Loudspeaker +              |                                             |
|  17 | NC                         | Not connected                               |
|  18 | GND                        |                                             |
|  19 | GND                        |                                             |
|  20 | Rotary encoder signal 1    | Closes to GND                               |
|  21 | Rotary encoder signal 2    | Closes to GND                               |
|  22 | ROW4                       | Keyboard matrix row 4                       |
|  23 | RTC battery +              | Battery negative terminal connects to GND   |
|  24 | NC                         | Not connected                               |
|  25 | GND                        |                                             |
|  26 | Earpiece speaker +         |                                             |
|  27 | Earpiece speaker −         |                                             |
|  28 | GND                        |                                             |

## Keyboard matrix

|          | COL0                   | COL1                 | COL2               | COL3 |
|----------|------------------------|----------------------|--------------------|------|
| **ROW0** | 1                      | 2                    | 3                  | CALL |
| **ROW1** | 4                      | 5                    | 6                  | Memo |
| **ROW2** | 7                      | 8                    | 9                  | C    |
| **ROW3** | \*                     | 0                    | #                  | —    |
| **ROW4** | Rotary wheel bend down | Rotary wheel bend up | Rotary wheel push  | —    |

"—" marks an unused matrix position.

The lid button, power button and rotary encoder signals are wired directly to
the connector and are not part of the matrix.
