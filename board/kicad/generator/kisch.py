"""Tiny KiCad schematic writer: places library symbols and connects pins with
short wire stubs ending in labels / power symbols."""
import copy, math, uuid as _uuid, re
from sexp import Sym, parse, dump, find, find1
from libinfo import get_symbol

PROJECT = "cmd-z5"
CUSTOM_LIB = "cmd-z5"
FONT = [Sym('font'), [Sym('size'), 1.27, 1.27]]

def U():
    return str(_uuid.uuid4())

def snap(v, g=1.27):
    return round(round(v / g) * g, 4)

# ----------------------------------------------------------------- library
class Library:
    def __init__(self):
        self.custom = {}   # name -> sexpr (top-level name without lib prefix)

    def add_custom(self, sym):
        self.custom[sym[1]] = sym

    def get(self, lib_id):
        lib, name = lib_id.split(':')
        if lib == CUSTOM_LIB:
            return copy.deepcopy(self.custom[name])
        return get_symbol(lib, name)

LIB = Library()

def sym_pins(sym):
    """pins grouped by unit: {unit: [pin dict]} (unit 0 = common)."""
    out = {}
    for sub in find(sym, 'symbol'):
        m = re.match(r'.*_(\d+)_(\d+)$', sub[1])
        unit = int(m.group(1))
        for e in sub:
            if isinstance(e, list) and e and e[0] == 'pin':
                at = find1(e, 'at')
                hide = any(isinstance(z, list) and z[0] == 'hide' and (len(z) == 1 or z[1] in ('yes', Sym('yes'))) for z in e) or Sym('hide') in e
                out.setdefault(unit, []).append(dict(
                    type=str(e[1]), x=float(at[1]), y=float(at[2]), ang=int(float(at[3])),
                    name=find1(e, 'name')[1], num=find1(e, 'number')[1], hide=hide))
    return out

def unit_count(sym):
    units = set(sym_pins(sym).keys()) - {0}
    return max(units) if units else 1

# ------------------------------------------------------------ geometry
def rot_vec(x, y, r):
    """screen coords (y down), rotate CCW by r degrees as seen on screen"""
    a = math.radians(r)
    c, s = round(math.cos(a), 6), round(math.sin(a), 6)
    return (x * c + y * s, -x * s + y * c)

def pin_screen(sx, sy, rot, p):
    dx, dy = rot_vec(p['x'], -p['y'], rot)
    return (round(sx + dx, 4), round(sy + dy, 4))

def pin_outward(rot, p):
    a = math.radians(p['ang'])
    vx, vy = math.cos(a), -math.sin(a)          # direction pin points (toward body)
    vx, vy = rot_vec(vx, vy, rot)
    return (round(-vx), round(-vy))

DIR_NAME = {(1, 0): 'right', (-1, 0): 'left', (0, -1): 'up', (0, 1): 'down'}

# ------------------------------------------------------------ power nets
# name -> (lib_id, kind) ; kind 'top' = body above pin, 'gnd' = body below
POWER = {
    'GND': ('power:GND', 'gnd'),
    '+3V0': ('power:+3V0', 'top'),
    'VBUS': ('power:VBUS', 'top'),
}
CUSTOM_POWER = ['VSYS', 'VBAT', '+3V0_RF', '+3V0_LCD', 'VAMP', '+5V_GPS', 'VBUS_NRF']
for n in CUSTOM_POWER:
    POWER[n] = (CUSTOM_LIB + ':' + n, 'top')

def make_power_symbols():
    base = get_symbol('power', '+3V0')
    for n in CUSTOM_POWER:
        s = copy.deepcopy(base)
        s[1] = n
        for e in s:
            if isinstance(e, list) and e[0] == 'property' and e[1] == 'Value':
                e[2] = n
            if isinstance(e, list) and e[0] == 'property' and e[1] == 'Description':
                e[2] = 'Power symbol creates a global label with name "%s"' % n
            if isinstance(e, list) and e[0] == 'symbol':
                e[1] = e[1].replace('+3V0', n)
        LIB.add_custom(s)

def power_rot(kind, d):
    if kind == 'top':
        return {(0, -1): 0, (-1, 0): 90, (0, 1): 180, (1, 0): 270}[d]
    return {(0, 1): 0, (1, 0): 90, (0, -1): 180, (-1, 0): 270}[d]

# ------------------------------------------------------------ project
class Project:
    def __init__(self):
        self.sheets = []
        self.refcount = {}
        self.pwrcount = 0
        self.root_uuid = U()

    def next_ref(self, prefix):
        self.refcount[prefix] = self.refcount.get(prefix, 0) + 1
        return "%s%d" % (prefix, self.refcount[prefix])

    def sheet(self, name, fname, title, paper='A3'):
        s = Sheet(self, name, fname, title, paper)
        self.sheets.append(s)
        return s

    def net_sheets(self):
        d = {}
        for s in self.sheets:
            for n in s.nets_used:
                d.setdefault(n, set()).add(s.name)
        return d


class Sheet:
    def __init__(self, prj, name, fname, title, paper):
        self.prj, self.name, self.fname, self.title, self.paper = prj, name, fname, title, paper
        self.uuid = U()
        self.items = []         # finished sexpr items
        self.lib_ids = set()
        self.pending_labels = []  # (net, x, y, dir)
        self.nets_used = set()
        self.unconnected = []
        self.texts = []

    # -------------------------------------------------------------- symbols
    def place(self, lib_id, at, rot=0, ref=None, value=None, unit=1, footprint=None,
              nets=None, nc=(), fields=None, dnp=False, ref_prefix=None, stub=2.54,
              hide_value=False, auto_nc=True, ref_pos=None, val_pos=None, in_bom=True, label_pins=()):
        sym = LIB.get(lib_id)
        self.lib_ids.add(lib_id)
        props = {p[1]: p for p in find(sym, 'property')}
        if ref is None:
            prefix = ref_prefix or props['Reference'][2]
            ref = self.prj.next_ref(prefix)
        if value is None:
            value = props['Value'][2]
        if footprint is None:
            footprint = props['Footprint'][2] if 'Footprint' in props else ''
        sx, sy = snap(at[0]), snap(at[1])
        pins = sym_pins(sym)
        upins = pins.get(0, []) + pins.get(unit, [])
        nets = dict(nets or {})
        # geometry for field placement
        pts = [pin_screen(sx, sy, rot, p) for p in upins] or [(sx, sy)]
        xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
        is_power = lib_id.startswith('power:') or lib_id.split(':')[1] in CUSTOM_POWER
        if ref_pos is None:
            if len(upins) <= 3 and not is_power:
                if rot in (0, 180):
                    ref_pos = (sx + 2.54, sy - 1.27, 'left'); val_pos = val_pos or (sx + 2.54, sy + 1.27, 'left')
                else:
                    ref_pos = (sx, sy - 2.54, 'center'); val_pos = val_pos or (sx, sy + 2.54, 'center')
            else:
                ref_pos = (min(xs) + 2.54, min(ys) - 3.81, 'left')
                val_pos = val_pos or (min(xs) + 2.54, max(ys) + 3.81, 'left')
        if val_pos is None:
            val_pos = (sx, sy + 2.54, 'center')

        def prop(name, val, pos, hide=False, just=None):
            eff = [Sym('effects'), copy.deepcopy(FONT)]
            if rot in (90, 180):   # text ends up at 180 deg: KiCad mirrors the justification
                just = {'left': 'right', 'right': 'left'}.get(just, just)
            if just and just != 'center':
                eff.append([Sym('justify'), Sym(just)])
            p = [Sym('property'), name, val, [Sym('at'), snap(pos[0], 0.01), snap(pos[1], 0.01), 90 if rot in (90, 270) else 0]]
            if hide:
                p.append([Sym('hide'), Sym('yes')])
            p.append(eff)
            return p

        it = [Sym('symbol'), [Sym('lib_id'), lib_id], [Sym('at'), sx, sy, rot], [Sym('unit'), unit],
              [Sym('exclude_from_sim'), Sym('no')], [Sym('in_bom'), Sym('yes' if in_bom and not is_power else 'no')],
              [Sym('on_board'), Sym('yes')], [Sym('dnp'), Sym('yes' if dnp else 'no')],
              [Sym('uuid'), U()]]
        it.append(prop('Reference', ref, ref_pos[:2], hide=is_power, just=ref_pos[2]))
        it.append(prop('Value', value, val_pos[:2], hide=hide_value, just=val_pos[2]))
        it.append(prop('Footprint', footprint, (sx, sy), hide=True))
        it.append(prop('Datasheet', props['Datasheet'][2] if 'Datasheet' in props else '', (sx, sy), hide=True))
        it.append(prop('Description', props['Description'][2] if 'Description' in props else '', (sx, sy), hide=True))
        for k, v in (fields or {}).items():
            it.append(prop(k, v, (sx, sy), hide=True))
        for p in pins.get(0, []) + [q for u, l in pins.items() if u != 0 for q in l]:
            it.append([Sym('pin'), p['num'], [Sym('uuid'), U()]])
        it.append([Sym('instances'), [Sym('project'), PROJECT,
                   [Sym('path'), "/%s/%s" % (self.prj.root_uuid, self.uuid),
                    [Sym('reference'), ref], [Sym('unit'), unit]]]])
        self.items.append(it)

        # ---- connections
        done = set()
        big = len(upins) > 8
        for p in upins:
            key = p['num']
            pos = pin_screen(sx, sy, rot, p)
            if key in nets:
                if (pos, nets[key]) in done:
                    continue
                done.add((pos, nets[key]))
                self.connect(pos, pin_outward(rot, p), nets[key], stub, pintype=p['type'], force_label=(key in label_pins or (big and not (nets[key] == 'GND' and pin_outward(rot, p) == (0, 1)))))
            elif key in nc:
                self.items.append([Sym('no_connect'), [Sym('at'), pos[0], pos[1]], [Sym('uuid'), U()]])
            elif not p['hide'] and p['type'] != 'no_connect':
                # visible but unassigned -> no connect (report)
                if (pos) in [d[0] for d in done]:
                    continue
                if auto_nc:
                    self.items.append([Sym('no_connect'), [Sym('at'), pos[0], pos[1]], [Sym('uuid'), U()]])
                    self.unconnected.append("%s.%s(%s)" % (ref, p['num'], p['name']))
        unknown = set(nets) - {p['num'] for p in upins}
        if unknown:
            raise ValueError("%s: unknown pins %s" % (ref, unknown))
        return ref

    def power(self, net, pos, d):
        lib_id, kind = POWER[net]
        self.prj.pwrcount += 1
        r = power_rot(kind, d)
        # value text position: beyond the symbol body
        off = {0: (0, -5.08), 90: (-5.08, 0), 180: (0, 5.08), 270: (5.08, 0)}[r] if kind == 'top' else \
              {0: (0, 5.08), 90: (5.08, 0), 180: (0, -5.08), 270: (-5.08, 0)}[r]
        just = 'center'
        if off[0] < 0: just = 'right'
        if off[0] > 0: just = 'left'
        self.place(lib_id, pos, rot=r, ref="#PWR%03d" % self.prj.pwrcount, value=net,
                   ref_pos=(pos[0], pos[1], 'center'), val_pos=(pos[0] + off[0], pos[1] + off[1], just), stub=0, auto_nc=False)

    def connect(self, pos, d, net, stub=2.54, pintype='passive', force_label=False):
        self.nets_used.add(net)
        end = (round(pos[0] + d[0] * stub, 4), round(pos[1] + d[1] * stub, 4))
        if stub:
            self.items.append([Sym('wire'), [Sym('pts'), [Sym('xy'), pos[0], pos[1]], [Sym('xy'), end[0], end[1]]],
                               [Sym('stroke'), [Sym('width'), 0], [Sym('type'), Sym('default')]], [Sym('uuid'), U()]])
        if net in POWER and not force_label:
            self.power(net, end, d)
        else:
            self.pending_labels.append((net, end, d, pintype))

    def flag(self, net, at):
        """PWR_FLAG next to a power symbol of the net"""
        n = self.prj.refcount.setdefault('#FLG', 0) + 1
        self.prj.refcount['#FLG'] = n
        top = POWER.get(net, ('', 'gnd'))[1] == 'top'
        r = 180 if top else 0
        vy = 3.81 if top else -3.81
        self.place('power:PWR_FLAG', at, rot=r, ref="#FLG%02d" % n, nets={'1': net},
                   ref_pos=(at[0], at[1], 'center'), val_pos=(at[0], at[1] + vy, 'center'))

    # convenience passives -------------------------------------------------
    def R(self, value, at, n1, n2, rot=0, fp='Resistor_SMD:R_0402_1005Metric', **kw):
        return self.place('Device:R_Small', at, rot=rot, value=value, footprint=fp, nets={'1': n1, '2': n2}, **kw)

    def C(self, value, at, n1, n2='GND', rot=0, fp='Capacitor_SMD:C_0402_1005Metric', **kw):
        return self.place('Device:C_Small', at, rot=rot, value=value, footprint=fp, nets={'1': n1, '2': n2}, **kw)

    def L(self, value, at, n1, n2, rot=90, fp='Inductor_SMD:L_0402_1005Metric', **kw):
        return self.place('Device:L_Small', at, rot=rot, value=value, footprint=fp, nets={'1': n1, '2': n2}, **kw)

    def text(self, s, at, size=1.27, bold=False):
        f = [Sym('font'), [Sym('size'), size, size]]
        if bold:
            f.append([Sym('bold'), Sym('yes')])
        self.items.append([Sym('text'), s, [Sym('exclude_from_sim'), Sym('no')], [Sym('at'), at[0], at[1], 0],
                           [Sym('effects'), f, [Sym('justify'), Sym('left'), Sym('top')]], [Sym('uuid'), U()]])

    def box(self, x1, y1, x2, y2, title=None):
        self.items.append([Sym('rectangle'), [Sym('start'), x1, y1], [Sym('end'), x2, y2],
                           [Sym('stroke'), [Sym('width'), 0], [Sym('type'), Sym('dash')]],
                           [Sym('fill'), [Sym('type'), Sym('none')]], [Sym('uuid'), U()]])
        if title:
            self.text(title, (x1 + 1.27, y1 + 1.27), size=2.0, bold=True)

    # -------------------------------------------------------------- output
    def finalize_labels(self, global_nets):
        for net, end, d, pintype in self.pending_labels:
            ang = {(1, 0): 0, (-1, 0): 180, (0, -1): 90, (0, 1): 270}[d]
            just = 'left' if ang in (0, 90) else 'right'
            if net in global_nets or net in POWER:
                shape = 'passive'
                self.items.append([Sym('global_label'), net, [Sym('shape'), Sym(shape)],
                                   [Sym('at'), end[0], end[1], ang], [Sym('fields_autoplaced'), Sym('yes')],
                                   [Sym('effects'), copy.deepcopy(FONT), [Sym('justify'), Sym(just)]],
                                   [Sym('uuid'), U()],
                                   [Sym('property'), 'Intersheetrefs', '${INTERSHEET_REFS}', [Sym('at'), end[0], end[1], 0],
                                    [Sym('hide'), Sym('yes')],
                                    [Sym('effects'), copy.deepcopy(FONT), [Sym('justify'), Sym('left')]]]])
            else:
                self.items.append([Sym('label'), net, [Sym('at'), end[0], end[1], ang],
                                   [Sym('fields_autoplaced'), Sym('yes')],
                                   [Sym('effects'), copy.deepcopy(FONT), [Sym('justify'), Sym(just), Sym('bottom')]],
                                   [Sym('uuid'), U()]])

    def lib_symbols(self):
        out = [Sym('lib_symbols')]
        for lid in sorted(self.lib_ids):
            s = LIB.get(lid)
            s[1] = lid
            out.append(s)
        return out

    def sexpr(self, page):
        return [Sym('kicad_sch'), [Sym('version'), 20250114], [Sym('generator'), 'eeschema'],
                [Sym('generator_version'), '9.0'], [Sym('uuid'), self.uuid], [Sym('paper'), self.paper],
                [Sym('title_block'), [Sym('title'), self.title], [Sym('company'), 'Sony CMD-Z5 Meshtastic replacement board'],
                 [Sym('comment'), 1, 'Generated schematic - verify against datasheets before layout']],
                self.lib_symbols()] + self.items
