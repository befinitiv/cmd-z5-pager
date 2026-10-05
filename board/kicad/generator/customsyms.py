"""Custom symbols for parts that are missing from the KiCad standard library."""
import copy
from sexp import Sym, find, find1
from libinfo import get_symbol
from kisch import LIB, FONT

def _prop(name, val, x, y, hide=False, just=None):
    eff = [Sym('effects'), copy.deepcopy(FONT)]
    if just:
        eff.append([Sym('justify'), Sym(just)])
    p = [Sym('property'), name, val, [Sym('at'), x, y, 0]]
    if hide:
        p.append([Sym('hide'), Sym('yes')])
    p.append(eff)
    return p

def _pin(num, name, etype, x, y, ang, hide=False, length=2.54):
    p = [Sym('pin'), Sym(etype), Sym('line'), [Sym('at'), x, y, ang], [Sym('length'), length]]
    if hide:
        p.append([Sym('hide'), Sym('yes')])
    p += [[Sym('name'), name, [Sym('effects'), copy.deepcopy(FONT)]],
          [Sym('number'), num, [Sym('effects'), copy.deepcopy(FONT)]]]
    return p

def ic(name, ref, value, footprint, datasheet, desc, left, right, top=(), bottom=(), width=20.32, lcsc=None):
    """left/right: list of (num, name, type) or None for a gap; top/bottom likewise.
    Pins on a 2.54 grid, length 2.54."""
    n = max(len(left), len(right))
    h = (n + 1) * 2.54
    half_w = width / 2
    y0 = round((n - 1) * 2.54 / 2 / 2.54) * 2.54   # top pin y (grid aligned)
    pins = []
    for i, p in enumerate(left):
        if p:
            pins.append(_pin(p[0], p[1], p[2], -half_w - 2.54, y0 - i * 2.54, 0, *(p[3:] if len(p) > 3 else ())))
    for i, p in enumerate(right):
        if p:
            pins.append(_pin(p[0], p[1], p[2], half_w + 2.54, y0 - i * 2.54, 180, *(p[3:] if len(p) > 3 else ())))
    top_y = y0 + 2.54
    bot_y = y0 - n * 2.54
    for i, p in enumerate(top):
        if p:
            x = (i - (len(top) - 1) / 2) * 2.54
            pins.append(_pin(p[0], p[1], p[2], round(x / 2.54) * 2.54, top_y + 2.54, 270, *(p[3:] if len(p) > 3 else ())))
    for i, p in enumerate(bottom):
        if p:
            x = (i - (len(bottom) - 1) / 2) * 2.54
            pins.append(_pin(p[0], p[1], p[2], round(x / 2.54) * 2.54, bot_y - 2.54, 90, *(p[3:] if len(p) > 3 else ())))
    body = [Sym('symbol'), name + "_0_1",
            [Sym('rectangle'), [Sym('start'), -half_w, top_y], [Sym('end'), half_w, bot_y],
             [Sym('stroke'), [Sym('width'), 0.254], [Sym('type'), Sym('default')]],
             [Sym('fill'), [Sym('type'), Sym('background')]]]]
    pinsym = [Sym('symbol'), name + "_1_1"] + pins
    s = [Sym('symbol'), name, [Sym('exclude_from_sim'), Sym('no')], [Sym('in_bom'), Sym('yes')],
         [Sym('on_board'), Sym('yes')],
         _prop('Reference', ref, -half_w, top_y + 1.27, just='left'),
         _prop('Value', value, -half_w, bot_y - 1.27, just='left'),
         _prop('Footprint', footprint, 0, 0, hide=True),
         _prop('Datasheet', datasheet, 0, 0, hide=True),
         _prop('Description', desc, 0, 0, hide=True)]
    if lcsc:
        s.append(_prop('LCSC', lcsc, 0, 0, hide=True))
    s += [body, pinsym]
    LIB.add_custom(s)


def build():
    ic('nPM1300-QEAA', 'U', 'nPM1300-QEAA-R7', 'Package_DFN_QFN:QFN-32-1EP_5x5mm_P0.5mm_EP3.45x3.45mm',
       'https://docs.nordicsemi.com/bundle/ps_npm1300/page/keyfeatures_html5.html',
       'Nordic nPM1300 PMIC: Li-ion charger, 2x 200 mA buck, 2x load switch/LDO, USB-C detection, QFN-32',
       left=[('21', 'VBUS', 'power_in'), ('23', 'CC1', 'bidirectional'), ('24', 'CC2', 'bidirectional'),
             ('22', 'VBUSOUT', 'power_out'), None,
             ('19', 'VBAT', 'power_in'), ('18', 'NTC', 'input'), None,
             ('15', 'SHPHLD', 'input'), ('17', 'VSET1', 'input'), ('16', 'VSET2', 'input'), None,
             ('12', 'VDDIO', 'power_in'), ('13', 'SDA', 'bidirectional'), ('14', 'SCL', 'input'), None,
             ('7', 'GPIO0', 'bidirectional'), ('8', 'GPIO1', 'bidirectional'), ('9', 'GPIO2', 'bidirectional'),
             ('10', 'GPIO3', 'bidirectional'), ('11', 'GPIO4', 'bidirectional')],
       right=[('20', 'VSYS', 'power_out'), ('4', 'PVDD', 'power_in'), None,
              ('3', 'SW1', 'output'), ('1', 'VOUT1', 'power_out'), ('2', 'PVSS1', 'power_in'), None,
              ('5', 'SW2', 'output'), ('32', 'VOUT2', 'power_out'), ('6', 'PVSS2', 'power_in'), None,
              ('28', 'LSIN1/VINLDO1', 'power_in'), ('29', 'LSOUT1/VOUTLDO1', 'power_out'),
              ('30', 'LSIN2/VINLDO2', 'power_in'), ('31', 'LSOUT2/VOUTLDO2', 'power_out'), None,
              ('25', 'LED0', 'open_collector'), ('26', 'LED1', 'open_collector'), ('27', 'LED2', 'open_collector')],
       bottom=[('33', 'AVSS', 'power_in')], width=30.48, lcsc='C7501206')

    ic('Wio-SX1262', 'U', 'Wio-SX1262', '',
       'https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262_Module_Datasheet.pdf',
       'Seeed Wio-SX1262 LoRa module (SX1262, TCXO on DIO3, RF switch on DIO2 + RF_SW), 11.6x11 mm, 12 pins',
       left=[('8', 'VCC', 'power_in'), None, ('5', 'NRST', 'input'), ('6', 'NSS', 'input'),
             ('4', 'SCK', 'input'), ('3', 'MOSI', 'input'), ('2', 'MISO', 'output'), None, ('1', 'RF_SW', 'input')],
       right=[('9', 'ANT', 'bidirectional'), None, None, None, ('11', 'BUSY', 'output'), ('12', 'DIO1', 'bidirectional')],
       bottom=[('7', 'GND', 'power_in'), ('10', 'GND', 'power_in')], width=17.78)

    ic('LIS2DH12', 'U', 'LIS2DH12TR', 'Package_LGA:LGA-12_2x2mm_P0.5mm',
       'https://www.st.com/resource/en/datasheet/lis2dh12.pdf',
       'ST LIS2DH12 3-axis accelerometer, I2C/SPI, LGA-12 2x2 mm',
       left=[('9', 'VDD', 'power_in'), ('10', 'VDD_IO', 'power_in'), None, ('1', 'SCL/SPC', 'input'),
             ('4', 'SDA/SDI/SDO', 'bidirectional'), ('2', 'CS', 'input'), ('3', 'SDO/SA0', 'bidirectional')],
       right=[('12', 'INT1', 'output'), ('11', 'INT2', 'output')],
       bottom=[('5', 'RES', 'passive'), ('6', 'GND', 'power_in'), ('7', 'GND', 'power_in'), ('8', 'GND', 'power_in')],
       width=20.32, lcsc='C110926')

    # DMG1016UDW: complementary pair, SOT-363: 1 S1, 2 G1, 3 D2, 4 S2, 5 G2, 6 D1 (Q1 = N, Q2 = P)
    s = get_symbol('Transistor_FET', 'Q_Dual_NMOS_PMOS_G1S2G2D2S1D1')
    old = 'Q_Dual_NMOS_PMOS_G1S2G2D2S1D1'
    remap = {(1, 'G'): '2', (1, 'S'): '1', (1, 'D'): '6', (2, 'G'): '5', (2, 'S'): '4', (2, 'D'): '3'}
    s[1] = 'DMG1016UDW'
    for e in s:
        if isinstance(e, list) and e[0] == 'property':
            if e[1] == 'Value': e[2] = 'DMG1016UDW'
            if e[1] == 'Footprint': e[2] = 'Package_TO_SOT_SMD:SOT-363_SC-70-6'
            if e[1] == 'Description': e[2] = 'Complementary N+P MOSFET pair, 20 V, SOT-363 (1 S1, 2 G1, 3 D2, 4 S2, 5 G2, 6 D1)'
            if e[1] == 'Datasheet': e[2] = 'https://www.diodes.com/assets/Datasheets/DMG1016UDW.pdf'
        if isinstance(e, list) and e[0] == 'symbol':
            unit = int(e[1].split('_')[-2])
            e[1] = e[1].replace(old, 'DMG1016UDW')
            for p in e:
                if isinstance(p, list) and p[0] == 'pin':
                    nm = find1(p, 'name')[1]
                    find1(p, 'number')[1] = remap[(unit, nm)]
    LIB.add_custom(s)

    # TPS610997 in DSBGA (YFF): VIN A1, GND A2, SW B1, VOUT B2, EN C1, FB C2
    s = get_symbol('Regulator_Switching', 'TPS61099DRV')
    remap = {'6': 'A1', '5': 'B1', '4': 'C1', '1': 'A2', '2': 'B2', '3': 'C2'}
    s[1] = 'TPS610997YFF'
    for e in s:
        if isinstance(e, list) and e[0] == 'property':
            if e[1] == 'Value': e[2] = 'TPS610997YFFR'
            if e[1] == 'Footprint': e[2] = 'cmd-z5:Texas_DSBGA-6_0.855x1.255mm_Layout2x3_P0.4mm_LevelB'
            if e[1] == 'Description': e[2] = 'Synchronous boost converter, fixed 5.0 V output, true load disconnect, DSBGA-6 (YFF)'
            if e[1] == 'Datasheet': e[2] = 'https://www.ti.com/lit/ds/symlink/tps61099.pdf'
    for e in s:
        if isinstance(e, list) and e[0] == 'symbol':
            e[1] = e[1].replace('TPS61099DRV', 'TPS610997YFF')
            keep = []
            for p in e:
                if isinstance(p, list) and p[0] == 'pin':
                    n = find1(p, 'number')
                    if n[1] == '7':
                        continue
                    n[1] = remap[n[1]]
                    if n[1] == 'B1':
                        p[1] = Sym('passive')
                keep.append(p)
            e[:] = keep
    LIB.add_custom(s)


def write_library(path):
    from sexp import dump
    lib = [Sym('kicad_symbol_lib'), [Sym('version'), 20241209], [Sym('generator'), 'cmd-z5-gen'],
           [Sym('generator_version'), '1.0']]
    for name in sorted(LIB.custom):
        lib.append(copy.deepcopy(LIB.custom[name]))
    open(path, 'w').write(dump(lib) + "\n")
