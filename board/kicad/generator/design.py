"""CMD-Z5 Meshtastic board schematic (generated). Run: python3 design.py <outdir>"""
import sys, os, json, copy
from sexp import Sym, dump
import kisch
from kisch import Project, U, make_power_symbols, PROJECT
import customsyms

OUT = sys.argv[1] if len(sys.argv) > 1 else 'out'
os.makedirs(OUT, exist_ok=True)

make_power_symbols()
customsyms.build()
P = Project()

C0402 = 'Capacitor_SMD:C_0402_1005Metric'
C0603 = 'Capacitor_SMD:C_0603_1608Metric'
R0402 = 'Resistor_SMD:R_0402_1005Metric'

# =====================================================================
# Sheet 1: power (USB-C, nPM1300, battery)
# =====================================================================
pw = P.sheet('Power', 'power.kicad_sch', 'Power: USB-C, nPM1300 PMIC, battery')

pw.box(10, 15, 120, 120, 'USB-C')
pw.place('Connector:USB_C_Receptacle_USB2.0_16P', (35, 65),
         footprint='Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal', value='USB4105-GF-A',
         fields={'LCSC': 'C3020560'},
         nets={'A4': 'VBUS', 'A5': 'USB_CC1', 'B5': 'USB_CC2', 'A6': 'USB_DP', 'B6': 'USB_DP',
               'A7': 'USB_DN', 'B7': 'USB_DN', 'A1': 'GND', 'SH': 'GND'}, nc=('A8', 'B8'))
pw.place('Power_Protection:USBLC6-2SC6', (90, 60), value='USBLC6-2SC6', fields={'LCSC': 'C7519'},
         nets={'1': 'USB_DP', '6': 'USB_DP', '3': 'USB_DN', '4': 'USB_DN', '5': 'VBUS', '2': 'GND'})
pw.text('ESD diodes sit directly on the D+/D- lines;\nroute connector -> USBLC6 -> nRF52840.', (65, 80))
pw.text('CC1/CC2 go straight to the nPM1300 (internal\npull-downs, no 5.1k resistors needed).', (65, 90))
pw.flag('VBUS', (100, 108))
pw.flag('GND', (112, 103))

pw.box(125, 15, 410, 200, 'nPM1300 PMIC')
pw.place('cmd-z5:nPM1300-QEAA', (215, 105), value='nPM1300-QEAA-R7', fields={'LCSC': 'C7501206'},
         nets={'21': 'VBUS', '23': 'USB_CC1', '24': 'USB_CC2', '22': 'VBUS_NRF',
               '19': 'VBAT', '18': 'PMIC_NTC', '15': 'SHPHLD', '17': 'GND', '16': 'PMIC_VSET2',
               '12': '+3V0', '13': 'I2C_SDA', '14': 'I2C_SCL', '7': 'PMIC_INT',
               '20': 'VSYS', '4': 'VSYS', '3': 'PMIC_SW1', '1': '+3V0_RF', '2': 'GND',
               '5': 'PMIC_SW2', '32': '+3V0', '6': 'GND',
               '28': '+3V0', '29': '+3V0_LCD', '30': '+3V0', '31': 'VAMP', '33': 'GND'},
         nc=('8', '9', '10', '11', '25', '26', '27'))

# input / output capacitors (Nordic reference configuration 1)
x = 140
pw.C('1u', (x, 35), 'VBUS', fp=C0603); pw.C('1u', (x + 10, 35), 'VBUS_NRF')
pw.C('2.2u', (x + 20, 35), 'VBAT', fp=C0603)
pw.C('100n', (x + 30, 35), '+3V0')
pw.text('VBUS     VBUSOUT   VBAT     VDDIO', (x - 3, 22))

x = 285
pw.C('10u', (x, 35), 'VSYS', fp=C0603); pw.C('10u', (x + 10, 35), 'VSYS', fp=C0603); pw.C('10u', (x + 20, 35), 'VSYS', fp=C0603)
pw.text('VSYS / PVDD (one per PVSS pad + one to GND)', (x - 3, 22))

# buck inductors and output caps
pw.L('2.2u', (285, 75), 'PMIC_SW1', '+3V0_RF', fp='Inductor_SMD:L_0805_2012Metric')
pw.C('10u', (305, 85), '+3V0_RF', fp=C0603)
pw.L('2.2u', (285, 105), 'PMIC_SW2', '+3V0', fp='Inductor_SMD:L_0805_2012Metric')
pw.C('10u', (305, 115), '+3V0', fp=C0603)
pw.text('BUCK1 -> +3V0_RF (Wio-SX1262), off at boot (VSET1 = GND),\nenabled over I2C by firmware.', (320, 72))
pw.text('BUCK2 -> +3V0 main rail, 3.0 V at boot (VSET2 = 150k).', (320, 104))
pw.text('Inductors: 2.2 uH, DCR < 400 mOhm, 2016 (0806) size.', (320, 115))

# load switch output caps
pw.C('1u', (300, 140), '+3V0_LCD'); pw.C('1u', (310, 140), 'VAMP')
pw.text('LSOUT1 -> +3V0_LCD (display supply)\nLSOUT2 -> VAMP (earpiece amplifier)\nBoth off in low-power mode.', (320, 135))

# VSET / NTC / pull-ups
pw.R('150k 1%', (150, 165), 'PMIC_VSET2', 'GND')
pw.R('0', (165, 165), 'PMIC_NTC', 'GND', fields={'Note': 'Battery without thermistor: tie NTC to GND, disable NTC in firmware'})
pw.text('VSET2: 150k = 3.0 V\nNTC: 0R to GND when the cell has\nno thermistor (configure in FW)', (140, 182))

# Battery
pw.box(10, 125, 120, 200, 'Battery')
pw.place('Connector_Generic:Conn_01x02', (30, 160), value='Battery 1S Li-ion',
         footprint='Connector_JST:JST_SH_SM02B-SRSS-TB_1x02-1MP_P1.00mm_Horizontal',
         nets={'1': 'VBAT', '2': 'GND'})
pw.flag('VBAT', (60, 150))
pw.text('Charging is disabled at power-up; firmware must\nset charge current and VTERM over I2C.', (15, 180))

# =====================================================================
# Sheet 2: MCU (nRF52840, crystals, RF, SWD, accelerometer)
# =====================================================================
mc = P.sheet('MCU', 'mcu.kicad_sch', 'MCU: nRF52840, crystals, BLE antenna, SWD, accelerometer')

nrf_nets = {
    # supply (circuit configuration 5: VDD from external 3.0 V, DC/DC REG1, USB)
    'B1': '+3V0', 'Y2': '+3V0', 'AB2': '+3V0', 'B7': 'GND', 'F23': 'GND',
    'AD2': 'VBUS_NRF', 'AC5': 'NRF_DECUSB', 'C1': 'NRF_DEC1', 'D23': 'NRF_DEC3',
    'B5': 'NRF_DEC4', 'E24': 'NRF_DEC4', 'B3': 'NRF_DCC',
    'B24': 'XC1', 'A23': 'XC2', 'D2': 'XL1', 'F2': 'XL2', 'H23': 'NRF_ANT',
    'AD4': 'USB_DN', 'AD6': 'USB_DP', 'AC24': 'SWDIO', 'AA24': 'SWDCLK', 'AC13': 'NRF_RESET',
    # GPIO assignment (README)
    'G1': 'LCD_D7', 'H2': 'SPK_PWM', 'L1': 'EAR_PWM', 'M2': 'VIBE_EN', 'N1': 'LED_EN',
    'P2': 'I2C_SDA', 'R1': 'I2C_SCL', 'T2': 'GPS_RXD', 'U1': 'GPS_TXD',
    'AD8': 'LORA_SCK', 'AC9': 'LORA_MOSI', 'AD10': 'LORA_MISO', 'AC11': 'LORA_NSS', 'AD12': 'LCD_A0',
    'AC15': 'LCD_D0', 'AD16': 'LCD_D1', 'AC17': 'LCD_D2', 'AD18': 'LCD_D3', 'AC19': 'LCD_D4',
    'AD20': 'LCD_D5', 'AC21': 'LCD_D6', 'AD22': 'LCD_WR',
    'L24': 'LORA_RF_SW', 'J24': 'LORA_NRST', 'Y23': 'LORA_BUSY', 'W24': 'LORA_DIO1',
    'V23': 'GPS_EN', 'U24': 'ACC_INT1', 'T23': 'PMIC_INT', 'R24': 'PWR_BTN', 'P23': 'LCD_RST',
    'A8': 'KB_COL0', 'B9': 'KB_COL1', 'A10': 'KB_COL2', 'B11': 'KB_COL3',
    'A12': 'KB_ROW0', 'B13': 'KB_ROW1', 'A14': 'KB_ROW2', 'B15': 'KB_ROW3', 'A16': 'KB_ROW4',
    'B17': 'ENC_A', 'B19': 'ENC_B', 'A20': 'LID_SW',
}
mc.place('MCU_Nordic:nRF52840', (205, 150), value='nRF52840-QIAA-R', fields={'LCSC': 'C190794'},
         nets=nrf_nets, nc=('J1', 'K2', 'A18', 'N24'))
mc.text('P0.04 / P0.05: spare (AIN2/AIN3)\nDEC2, DEC5: not connected (aQFN73 build code Fxx and later)', (150, 236))

# decoupling (Nordic reference circuit configuration no. 5)
mc.box(10, 15, 140, 75, 'nRF52840 supply and decoupling')
x, y = 18, 40
for v, n, fp in [('4.7u', '+3V0', C0402), ('1u', '+3V0', C0402), ('100n', '+3V0', C0402), ('100n', '+3V0', C0402),
                 ('100n', '+3V0', C0402), ('100n', 'NRF_DEC1', C0402), ('100p', 'NRF_DEC3', C0402),
                 ('1u', 'NRF_DEC4', C0402), ('47n', 'NRF_DEC4', C0402), ('4.7u', 'NRF_DECUSB', C0402),
                 ('4.7u', 'VBUS_NRF', C0603)]:
    mc.C(v, (x, y), n, fp=fp)
    x += 11
mc.L('10u', (45, 65), 'NRF_DCC', 'NRF_DCC_L', fp='Inductor_SMD:L_0603_1608Metric')
mc.L('15n', (70, 65), 'NRF_DCC_L', 'NRF_DEC4', fp='Inductor_SMD:L_0402_1005Metric')
mc.text('DC/DC REG1: DCC -> 10 uH -> 15 nH -> DEC4/DEC6', (90, 63))

# crystals
mc.box(10, 80, 140, 135, 'Crystals')
mc.place('Device:Crystal_GND24', (40, 100), value='32 MHz CL=10pF', footprint='Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm',
         fields={'LCSC': 'C91742', 'MPN': 'X1E0000210163'}, nets={'1': 'XC1', '3': 'XC2', '2': 'GND'},
         ref_pos=(36, 93, 'left'), val_pos=(48, 104, 'left'))
mc.C('15p', (25, 115), 'XC1'); mc.C('15p', (55, 115), 'XC2')
mc.place('Device:Crystal_Small', (100, 100), value='32.768 kHz CL=7pF', footprint='Crystal:Crystal_SMD_2012-2Pin_2.0x1.2mm',
         fields={'LCSC': 'C99009', 'MPN': 'X1A0000610006'}, nets={'1': 'XL1', '2': 'XL2'})
mc.C('8.2p', (90, 115), 'XL1'); mc.C('8.2p', (110, 115), 'XL2')
mc.text('Load caps: C = 2*CL - ~4 pF stray; tune on prototype.', (15, 127))

# RF
mc.box(10, 140, 140, 200, 'BLE antenna')
mc.L('4.7n', (40, 160), 'NRF_ANT', 'BLE_RF1', fp='Inductor_SMD:L_0402_1005Metric')
mc.C('0.8p', (30, 170), 'NRF_ANT'); mc.C('0.5p', (50, 170), 'BLE_RF1')
mc.R('0', (70, 160), 'BLE_RF1', 'BLE_RF2', rot=90)
mc.C('DNP', (62, 170), 'BLE_RF1', dnp=True); mc.C('DNP', (80, 170), 'BLE_RF2', dnp=True)
mc.place('Device:Antenna_Chip', (100, 165), value='2450AT18A100E',
         footprint='RF_Antenna:Johanson_2450AT18x100', fields={'LCSC': 'C89334'},
         nets={'1': 'BLE_RF2'}, nc=('2',))
mc.text('Nordic harmonic filter (4.7 nH, 0.8 pF, 0.5 pF),\nthen pi network for the chip antenna (tune).', (15, 185))

# SWD
mc.box(10, 205, 140, 285, 'Debug (SWD)')
mc.place('Connector_Generic:Conn_01x05', (45, 245), value='SWD', dnp=True,
         footprint='Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical',
         nets={'1': '+3V0', '2': 'SWDIO', '3': 'SWDCLK', '4': 'NRF_RESET', '5': 'GND'})
mc.text('1x5 2.54 mm header, not fitted (DNP):\n1 +3V0  2 SWDIO  3 SWDCLK  4 RESET  5 GND', (15, 265))

# accelerometer
mc.box(285, 15, 410, 105, 'Accelerometer')
mc.place('cmd-z5:LIS2DH12', (340, 55), value='LIS2DH12TR', fields={'LCSC': 'C110926'},
         nets={'9': '+3V0', '10': '+3V0', '1': 'I2C_SCL', '4': 'I2C_SDA', '2': '+3V0', '3': '+3V0',
               '12': 'ACC_INT1', '5': 'GND', '6': 'GND', '7': 'GND', '8': 'GND'}, nc=('11',))
mc.C('100n', (300, 85), '+3V0'); mc.C('1u', (310, 85), '+3V0')
mc.text('CS high = I2C. SA0 tied high (I2C address 0x19):\nits internal pull-up draws current if tied low.', (330, 80))

# I2C pull-ups
mc.box(285, 110, 410, 160, 'I2C')
mc.R('4.7k', (300, 135), '+3V0', 'I2C_SDA'); mc.R('4.7k', (315, 135), '+3V0', 'I2C_SCL')
mc.text('Shared by nPM1300 and LIS2DH12.', (330, 130))

# =====================================================================
# Sheet 3: radio + GPS
# =====================================================================
rg = P.sheet('Radio_GPS', 'radio_gps.kicad_sch', 'LoRa (Wio-SX1262) and GPS (TBS M10N)')

rg.box(10, 15, 200, 130, 'LoRa: Wio-SX1262')
rg.place('cmd-z5:Wio-SX1262', (80, 65), footprint='cmd-z5:Seeed_Wio-SX1262',
         nets={'8': '+3V0_RF', '5': 'LORA_NRST', '6': 'LORA_NSS', '4': 'LORA_SCK', '3': 'LORA_MOSI',
               '2': 'LORA_MISO', '1': 'LORA_RF_SW', '9': 'LORA_ANT', '11': 'LORA_BUSY', '12': 'LORA_DIO1',
               '7': 'GND', '10': 'GND'})
rg.C('10u', (30, 105), '+3V0_RF', fp=C0603); rg.C('100n', (40, 105), '+3V0_RF')
rg.R('0', (130, 50), 'LORA_ANT', 'LORA_ANT2', rot=90)
rg.C('DNP', (120, 60), 'LORA_ANT', dnp=True); rg.C('DNP', (140, 60), 'LORA_ANT2', dnp=True)
rg.place('Device:Antenna', (165, 45), value='PCB antenna 868/915 MHz',
         footprint='RF_Antenna:Texas_SWRA416_868MHz_915MHz', nets={'1': 'LORA_ANT2'})
rg.text('Standard Wio-SX1262 uses its IPEX connector; the ANT pad\nis only connected on the RF SMT-pin version (ask Seeed).\nPi network: 0R + 2x DNP for antenna tuning.', (100, 85))
rg.text('Meshtastic: SX126X_RXEN = RF_SW, DIO2 as RF switch,\nTCXO 1.8 V on DIO3.', (100, 105))

rg.box(10, 140, 200, 230, 'GPS: TBS M10N (5 V, switched)')
rg.place('cmd-z5:TPS610997YFF', (60, 180), fields={'LCSC': 'C2072359'},
         nets={'A1': 'VSYS', 'B1': 'GPS_SW', 'C1': 'GPS_EN', 'A2': 'GND', 'B2': '+5V_GPS', 'C2': 'GND'})
rg.L('2.2u', (60, 160), 'VSYS', 'GPS_SW', fp='Inductor_SMD:L_0805_2012Metric')
rg.C('10u', (25, 195), 'VSYS', fp=C0603)
rg.C('10u', (90, 200), '+5V_GPS', fp=C0603); rg.C('10u', (100, 200), '+5V_GPS', fp=C0603)
rg.R('1M', (35, 210), 'GPS_EN', 'GND')
rg.place('Connector_Generic:Conn_01x04', (150, 180), value='TBS M10N',
         footprint='Connector_JST:JST_SH_SM04B-SRSS-TB_1x04-1MP_P1.00mm_Horizontal',
         nets={'1': '+5V_GPS', '2': 'GND', '3': 'GPS_TX_M', '4': 'GPS_TXD'})
rg.R('1k', (125, 205), 'GPS_TX_M', 'GPS_RXD', rot=90)
rg.text('GPS_TXD (nRF TX) -> GPS RX; GPS TX -> 1k -> nRF RX.\nCheck the M10N cable pin order before ordering.\nEN low = GPS fully unpowered (boost load disconnect).', (110, 215))

# =====================================================================
# Sheet 4: UI (display, keyboard flex, audio, motor, LEDs, buttons)
# =====================================================================
P.refcount['Q'] = 2
ui = P.sheet('UI', 'ui.kicad_sch', 'User interface: display, keyboard flex, audio, vibration, LEDs')

ui.box(10, 15, 140, 160, 'Display (26-pin FFC, top contact)')
lcd = {'1': 'GND', '2': 'GND', '3': '+3V0_LCD', '4': '+3V0_LCD', '5': 'GND', '6': '+3V0_LCD', '7': 'GND',
       '8': '+3V0_LCD', '9': 'GND', '10': 'LCD_WR', '11': '+3V0_LCD', '12': 'LCD_RST', '13': 'LCD_A0',
       '14': 'GND', '15': '+3V0_LCD', '16': '+3V0_LCD', '17': 'LCD_D7', '18': 'LCD_D6', '19': 'LCD_D5',
       '20': 'LCD_D4', '21': 'LCD_D3', '22': 'LCD_D2', '23': 'LCD_D1', '24': 'LCD_D0', '25': '+3V0_LCD',
       '26': 'GND', 'MP': 'GND'}
ui.place('Connector_Generic_MountingPin:Conn_01x26_MountingPin', (40, 85), value='KH-FPC0.5-H1.0SMT-26P-QCHF',
         footprint='cmd-z5:FPC-SMD_26P-P0.50_KH-FPC0.5-H1.0SMT-26P-QCHF',
         fields={'LCSC': 'C3250122'}, nets=lcd)
ui.C('1u', (95, 40), '+3V0_LCD'); ui.C('100n', (105, 40), '+3V0_LCD')
ui.text('Pin 9 -> GND, pin 14 (/CS) -> GND,\npin 15 (CS) -> +3V0_LCD.\nWhen +3V0_LCD is off, drive all\nLCD_* lines low (no back-powering).', (80, 60))

ui.box(150, 15, 410, 160, 'Keyboard flex (28-pin FFC, bottom contact)')
kb = {'1': 'LID_SW', '2': 'KB_COL0', '3': 'KB_COL1', '4': 'KB_COL2', '5': 'KB_COL3', '6': 'KB_ROW0',
      '7': 'KB_ROW1', '8': 'KB_ROW2', '9': 'KB_ROW3', '10': 'PWR_KEY', '11': 'GND', '12': 'LED_K',
      '13': 'VSYS', '14': 'VSYS', '15': 'VIBE', '16': 'SPK', '18': 'GND', '19': 'GND',
      '20': 'ENC_A', '21': 'ENC_B', '22': 'KB_ROW4', '25': 'GND', '26': 'EAR_P', '27': 'EAR_N', '28': 'GND',
      'MP': 'GND'}
ui.place('Connector_Generic_MountingPin:Conn_01x28_MountingPin', (180, 85), value='KH-FPC0.5-H1.0SMT-28P-QCHF',
         footprint='cmd-z5:FPC-SMD_28P-P0.50_KH-FPC0.5-H1.0SMT-28P-QCHF',
         fields={'LCSC': 'C3250123'}, nets=kb, nc=('17', '23', '24'))
ui.text('Pin 23 = RTC battery + (not used; the nRF52840\nhas no backup domain). Pins 17, 24: NC on flex.', (215, 150))
ui.R('10M', (240, 40), '+3V0', 'LID_SW')
ui.R('10M', (255, 40), '+3V0', 'ENC_A'); ui.R('10M', (270, 40), '+3V0', 'ENC_B')
ui.text('Lid / encoder pull-ups: 10M so a closed contact\ncosts only 0.3 uA (disable the internal pull-ups\nin sleep). Pins 13/14 = LED + loudspeaker common.', (285, 33))

# power button -> nPM1300 SHPHLD and nRF GPIO
ui.place('Diode:BAT54C', (255, 80), value='BAT54C', fields={'LCSC': 'C916424'},
         nets={'1': 'SHPHLD', '2': 'PWR_BTN', '3': 'PWR_KEY'})
ui.text('Power key (flex pin 10, other side GND):\nwakes the nPM1300 from ship mode (SHPHLD,\n50k pull-up to VBAT) and is read by the nRF.', (285, 70))

# LED and loudspeaker low-side switches
ui.place('Transistor_FET:Q_Dual_NMOS_S1G1D2S2G2D1', (265, 120), unit=1, value='DMG1024UV',
         footprint='Package_TO_SOT_SMD:SOT-563', fields={'LCSC': 'C2759893'}, ref='Q1',
         nets={'1': 'GND', '2': 'LED_EN', '6': 'LED_K_SW'}, ref_pos=(270, 119, 'left'), val_pos=(270, 122, 'left'))
ui.place('Transistor_FET:Q_Dual_NMOS_S1G1D2S2G2D1', (325, 120), unit=2, value='DMG1024UV',
         footprint='Package_TO_SOT_SMD:SOT-563', fields={'LCSC': 'C2759893'}, ref='Q1',
         nets={'4': 'GND', '5': 'SPK_PWM', '3': 'SPK_SW'}, ref_pos=(330, 119, 'left'), val_pos=(330, 122, 'left'))
ui.R('0', (285, 100), 'LED_K', 'LED_K_SW', fields={'Note': 'LED current limit, value depends on flex LEDs'})
ui.R('10', (345, 100), 'SPK', 'SPK_SW', fp='Resistor_SMD:R_0603_1608Metric',
     fields={'Note': 'Loudspeaker current limit, adjust to speaker impedance'})
ui.R('100k', (250, 135), 'LED_EN', 'GND'); ui.R('100k', (310, 135), 'SPK_PWM', 'GND')
ui.place('Diode:1N5819WS', (362, 120), rot=270, fields={'LCSC': 'C191023'}, nets={'1': 'VSYS', '2': 'SPK'},
         ref_pos=(365, 119, 'left'), val_pos=(365, 122, 'left'))
ui.text('LED and loudspeaker: low-side switches,\ncommon (pins 13/14) = VSYS.', (365, 135))

# vibration motor high-side switch
ui.box(150, 165, 410, 285, 'Vibration motor (high-side switch)')
ui.place('cmd-z5:DMG1016UDW', (200, 240), unit=1, fields={'LCSC': 'C211433'}, ref='Q2',
         nets={'2': 'VIBE_EN', '1': 'GND', '6': 'VIBE_G'}, ref_pos=(205, 239, 'left'), val_pos=(205, 242, 'left'))
ui.place('cmd-z5:DMG1016UDW', (260, 205), rot=180, unit=2, fields={'LCSC': 'C211433'}, ref='Q2',
         nets={'5': 'VIBE_G', '4': 'VSYS', '3': 'VIBE'}, ref_pos=(255, 204, 'right'), val_pos=(255, 207, 'right'))
ui.R('100k', (185, 255), 'VIBE_EN', 'GND')
ui.R('100k', (285, 195), 'VSYS', 'VIBE_G')
ui.place('Diode:1N5819WS', (305, 235), rot=270, fields={'LCSC': 'C191023'}, nets={'1': 'VIBE', '2': 'GND'},
         ref_pos=(308, 234, 'left'), val_pos=(308, 237, 'left'))
ui.text('Motor: flex pin 15 (+), other terminal on GND.\nPWM VIBE_EN to set the strength.', (320, 210))

# earpiece amplifier
ui.box(10, 165, 140, 285, 'Earpiece amplifier')
ui.place('Amplifier_Audio:PAM8302AAS', (90, 225), value='PAM8302AASCR',
         footprint='Package_SO:MSOP-8_3x3mm_P0.65mm', fields={'LCSC': 'C113367'},
         nets={'1': 'VAMP', '3': 'EAR_IN', '4': 'EAR_INN', '5': 'EAR_P', '6': 'VAMP', '7': 'GND', '8': 'EAR_N'})
ui.R('1k', (30, 200), 'EAR_PWM', 'EAR_RC', rot=90)
ui.C('10n', (52, 215), 'EAR_RC')
ui.C('100n', (75, 200), 'EAR_RC', 'EAR_IN', rot=90)
ui.C('100n', (55, 245), 'EAR_INN', 'GND')
ui.C('10u', (115, 255), 'VAMP', fp=C0603); ui.C('100n', (125, 255), 'VAMP')
ui.text('PWM -> RC low-pass -> AC coupling. Amplifier is\npowered from nPM1300 LSOUT2 (VAMP), SD tied to VAMP.\nDrive EAR_PWM low while VAMP is off.', (15, 265))


# =====================================================================
# write files
# =====================================================================
glob = {n for n, s in P.net_sheets().items() if len(s) > 1}
warn = []
for s in P.sheets:
    s.finalize_labels(glob)
    if s.unconnected:
        warn.append((s.name, s.unconnected))
# nets that appear only once in the whole design (dangling labels)
cnt = {}
for s in P.sheets:
    for net, *_ in s.pending_labels:
        cnt[net] = cnt.get(net, 0) + 1
single = sorted(n for n, c in cnt.items() if c == 1)

for i, s in enumerate(P.sheets):
    open(os.path.join(OUT, s.fname), 'w').write(dump(s.sexpr(i + 2)) + "\n")

# root sheet
root = [Sym('kicad_sch'), [Sym('version'), 20250114], [Sym('generator'), 'eeschema'], [Sym('generator_version'), '9.0'],
        [Sym('uuid'), P.root_uuid], [Sym('paper'), 'A4'],
        [Sym('title_block'), [Sym('title'), 'Sony CMD-Z5 Meshtastic replacement board'],
         [Sym('comment'), 1, 'nRF52840 + Wio-SX1262 + TBS M10N + nPM1300'],
         [Sym('comment'), 2, 'Generated schematic - verify against datasheets before layout']],
        [Sym('lib_symbols')]]
for i, s in enumerate(P.sheets):
    x, y = 30 + (i % 2) * 120, 40 + (i // 2) * 60
    root.append([Sym('sheet'), [Sym('at'), x, y], [Sym('size'), 90, 40], [Sym('exclude_from_sim'), Sym('no')],
                 [Sym('in_bom'), Sym('yes')], [Sym('on_board'), Sym('yes')], [Sym('dnp'), Sym('no')],
                 [Sym('fields_autoplaced'), Sym('yes')],
                 [Sym('stroke'), [Sym('width'), 0.1524], [Sym('type'), Sym('solid')]],
                 [Sym('fill'), [Sym('color'), 0, 0, 0, 0.0]], [Sym('uuid'), s.uuid],
                 [Sym('property'), 'Sheetname', s.name, [Sym('at'), x, y - 0.7, 0],
                  [Sym('effects'), copy.deepcopy(kisch.FONT), [Sym('justify'), Sym('left'), Sym('bottom')]]],
                 [Sym('property'), 'Sheetfile', s.fname, [Sym('at'), x, y + 40.6, 0],
                  [Sym('effects'), copy.deepcopy(kisch.FONT), [Sym('justify'), Sym('left'), Sym('top')]]],
                 [Sym('instances'), [Sym('project'), PROJECT, [Sym('path'), '/' + P.root_uuid, [Sym('page'), str(i + 2)]]]]])
root.append([Sym('text'), 'Block overview: see README.md (component selection, power architecture, GPIO assignment).',
             [Sym('exclude_from_sim'), Sym('no')], [Sym('at'), 30, 165, 0],
             [Sym('effects'), copy.deepcopy(kisch.FONT), [Sym('justify'), Sym('left')]], [Sym('uuid'), U()]])
root.append([Sym('sheet_instances'), [Sym('path'), '/', [Sym('page'), '1']]])
root.append([Sym('embedded_fonts'), Sym('no')])
open(os.path.join(OUT, PROJECT + '.kicad_sch'), 'w').write(dump(root) + "\n")

customsyms.write_library(os.path.join(OUT, PROJECT + '.kicad_sym'))
open(os.path.join(OUT, 'fp-lib-table'), 'w').write(
    '(fp_lib_table\n\t(version 7)\n\t(lib (name "%s")(type "KiCad")(uri "${KIPRJMOD}/%s.pretty")(options "")(descr "CMD-Z5 custom footprints"))\n)\n' % (PROJECT, PROJECT))
open(os.path.join(OUT, 'sym-lib-table'), 'w').write(
    '(sym_lib_table\n\t(version 7)\n\t(lib (name "%s")(type "KiCad")(uri "${KIPRJMOD}/%s.kicad_sym")(options "")(descr "CMD-Z5 custom symbols"))\n)\n' % (PROJECT, PROJECT))
pro = {"meta": {"filename": PROJECT + ".kicad_pro", "version": 3},
       "sheets": [[P.root_uuid, "Root"]] + [[s.uuid, s.name] for s in P.sheets]}
if not os.path.exists(os.path.join(OUT, PROJECT + '.kicad_pro')):
    open(os.path.join(OUT, PROJECT + '.kicad_pro'), 'w').write(json.dumps(pro, indent=2) + "\n")

print("unassigned pins (auto no-connect):", warn)
print("single-use labels:", single)
