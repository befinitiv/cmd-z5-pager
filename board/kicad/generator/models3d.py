"""3D models (STEP) for custom footprints that have no vendor model.
Needs CadQuery: python3 -m venv venv && venv/bin/pip install cadquery
Run: venv/bin/python models3d.py <cmd-z5.3dshapes dir>

Coordinates are KiCad model coordinates: origin = footprint origin, x right,
y up (= -y of the footprint), z up from the board surface, mm.
"""
import sys, os
import cadquery as cq

BLACK = cq.Color(0.1, 0.1, 0.1)
GOLD = cq.Color(0.85, 0.65, 0.2)
TIN = cq.Color(0.8, 0.8, 0.82)


def spring(x, face_y, width=0.8, t=0.12):
    """side-push spring contact leaving the front face (at model y = face_y,
    pointing towards -y). Profile drawn in the YZ plane, contact point 3.3 mm
    above the board and 1.8 mm in front of the face (free position)."""
    out = -1.0  # springs point towards -y in model coordinates
    pts = [(-0.6, 1.0), (-0.6, 1.6), (0.6, 2.4), (1.6, 3.1), (1.8, 3.3), (1.6, 3.5),
           (0.6, 4.1), (-0.2, 4.3), (-0.5, 4.3)]
    pts = [(face_y + out * d, z) for d, z in pts]
    prof = cq.Workplane('YZ', origin=(x - width / 2, 0, 0)).polyline(pts).offset2D(t / 2)
    return prof.extrude(width)


def bat_2p_p3_00():
    """G-Switch GT-BTP30002-0500B-015A: 2 pins, 3.0 mm pitch, body 6.0 x 3.5 x 5.0 mm"""
    w, d, h, pitch = 6.0, 3.5, 5.0, 3.0
    front = -d / 2  # model y of the spring face (footprint +y side)
    xs = [-pitch / 2, pitch / 2]
    body = cq.Workplane('XY').box(w, d, h, centered=(True, True, False)).edges('|Z').fillet(0.2)
    for x in xs:   # pockets for the springs in the front face
        body = body.cut(cq.Workplane('XY').box(1.1, 1.2, 3.8, centered=(True, True, False))
                        .translate((x, front + 0.6 - 0.001, 0.6)))
    contacts = None
    for x in xs:
        tail = cq.Workplane('XY').box(0.8, 1.6, 0.12, centered=(True, True, False)).translate((x, d / 2 + 0.9 - 0.8, 0))
        riser = cq.Workplane('XY').box(0.8, 0.12, 1.0, centered=(True, True, False)).translate((x, front + 0.6, 0))
        base = cq.Workplane('XY').box(0.8, d - 0.6, 0.12, centered=(True, False, False)).translate((x, front + 0.6, 0))
        c = spring(x, front).union(tail).union(riser).union(base)
        contacts = c if contacts is None else contacts.union(c)
    # fixing foot: 0.2 mm plate in the centre slot, 1.0 mm into the board
    foot = cq.Workplane('XY').box(0.2, 1.2, 1.6, centered=(True, True, False)).translate((0, -0.85, -1.0))
    a = cq.Assembly(name='GT-BTP30002-0500B-015A')
    a.add(body, name='housing', color=BLACK)
    a.add(contacts, name='contacts', color=GOLD)
    a.add(foot, name='foot', color=TIN)
    return a


GREEN = cq.Color(0.05, 0.35, 0.15)
WHITE = cq.Color(0.9, 0.88, 0.8)


def wio_sx1262():
    """Seeed Wio-SX1262: 11.6 x 11 mm, 2.95 mm high, 0.8 mm PCB with gull-wing leads,
    L-shaped shield can, IPEX (U.FL) connector in the corner next to pins 9-12.
    Footprint pads 1-8 at footprint y = +5.5 (model y = -5.5), pads 9-12 at model y = +5.5."""
    w, l = 11.6, 11.0
    z_pcb, t_pcb, h = 0.15, 0.8, 2.95
    pcb = cq.Workplane('XY').box(w, l, t_pcb, centered=(True, True, False)).translate((0, 0, z_pcb))
    z_top = z_pcb + t_pcb
    ipex_x, ipex_y = 3.8, 3.8                     # model coordinates (footprint y is negated)
    cut_x0, cut_y0 = 2.5, 1.9                     # shield leaves this corner free for the IPEX
    shield = (cq.Workplane('XY').box(w - 0.3, l - 0.3, h - z_top, centered=(True, True, False))
              .translate((0, 0, z_top))
              .cut(cq.Workplane('XY').box(w, l, h, centered=(False, False, False)).translate((cut_x0, cut_y0, z_top - 0.01))))
    ipex = (cq.Workplane('XY').box(2.6, 2.6, 0.5, centered=(True, True, False)).translate((ipex_x, ipex_y, z_top)))
    post = (cq.Workplane('XY').circle(1.0).circle(0.8).extrude(0.75).translate((ipex_x, ipex_y, z_top + 0.5))
            .union(cq.Workplane('XY').circle(0.2).extrude(0.7).translate((ipex_x, ipex_y, z_top + 0.5))))
    leads = None
    pins = [(-4.49 + i * 1.27, -1) for i in range(8)] + [(0.59 - i * 1.27, 1) for i in range(4)]
    for x, s in pins:
        e = s * l / 2   # module edge
        top = cq.Workplane('XY').box(0.4, 0.45, 0.15, centered=(True, False, False)).translate((x, 0, z_pcb + 0.35))
        drop = cq.Workplane('XY').box(0.4, 0.15, z_pcb + 0.5, centered=(True, False, False)).translate((x, 0.3, 0))
        foot = cq.Workplane('XY').box(0.4, 0.55, 0.15, centered=(True, False, False)).translate((x, 0.3, 0))
        lead = top.union(drop).union(foot)
        if s < 0:
            lead = lead.mirror('XZ')
        lead = lead.translate((0, e - s * 0.15, 0))
        leads = lead if leads is None else leads.union(lead)
    a = cq.Assembly(name='Wio-SX1262')
    a.add(pcb, name='pcb', color=GREEN)
    a.add(shield, name='shield', color=TIN)
    a.add(ipex, name='ipex', color=WHITE)
    a.add(post, name='ipex_post', color=GOLD)
    a.add(leads, name='leads', color=GOLD)
    return a


DIE = cq.Color(0.15, 0.15, 0.17)
CERAMIC = cq.Color(0.92, 0.88, 0.78)


def dsbga6_yff():
    """TI DSBGA-6 (YFF), 0.855 x 1.255 mm, 2x3 balls at 0.4 mm pitch, 0.5 mm high.
    Ball A1 at footprint (-0.2, -0.4) = model (-0.2, +0.4)."""
    ball_d, z_die, h = 0.25, 0.19, 0.5
    die = cq.Workplane('XY').box(0.855, 1.255, h - z_die, centered=(True, True, False)).translate((0, 0, z_die))
    dot = cq.Workplane('XY').circle(0.06).extrude(0.005).translate((-0.25, 0.45, h))
    balls = None
    for x in (-0.2, 0.2):
        for y in (-0.4, 0.0, 0.4):
            s = cq.Workplane('XY').sphere(ball_d / 2).translate((x, y, ball_d / 2 - 0.02))
            balls = s if balls is None else balls.union(s)
    balls = balls.intersect(cq.Workplane('XY').box(2, 2, z_die + 0.01, centered=(True, True, False)))
    a = cq.Assembly(name='DSBGA-6')
    a.add(die, name='die', color=DIE)
    a.add(dot, name='pin1', color=cq.Color(0.8, 0.8, 0.8))
    a.add(balls, name='balls', color=TIN)
    return a


def crystal_2012():
    """2.0 x 1.2 x 0.6 mm 2-pad crystal (32.768 kHz): ceramic base, metal lid,
    terminals at the short ends over the footprint pads (x = +/-0.7)."""
    base = cq.Workplane('XY').box(2.0, 1.2, 0.25, centered=(True, True, False)).translate((0, 0, 0.02))
    lid = cq.Workplane('XY').box(1.8, 1.0, 0.33, centered=(True, True, False)).edges('|Z').fillet(0.1).translate((0, 0, 0.27))
    pads = None
    for x in (-0.75, 0.75):
        p = cq.Workplane('XY').box(0.5, 1.0, 0.03, centered=(True, True, False)).translate((x, 0, 0))
        pads = p if pads is None else pads.union(p)
    a = cq.Assembly(name='Crystal_2012')
    a.add(base, name='base', color=CERAMIC)
    a.add(lid, name='lid', color=TIN)
    a.add(pads, name='terminals', color=GOLD)
    return a


def fpc_26p_kh():
    """Kinghelm KH-FPC0.5-H1.0SMT-26P-QCHF, derived from the vendor 28-pin STEP of the same
    series: a 1.0 mm slice (two contacts at x = +/-0.25) is removed from the middle."""
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(here, '..', 'cmd-z5.3dshapes', 'CONN-SMD_28P-P0.50_KH-FPC0.5-H1.0SMT-28P-QCHF.step')
    s = cq.importers.importStep(src)
    left = s.intersect(cq.Workplane('XY').box(20, 20, 20, centered=(False, True, True)).translate((-20.5, 0, 0)))
    right = s.intersect(cq.Workplane('XY').box(20, 20, 20, centered=(False, True, True)).translate((0.5, 0, 0)))
    body = left.translate((0.5, 0, 0)).union(right.translate((-0.5, 0, 0)))
    a = cq.Assembly(name='KH-FPC0.5-H1.0SMT-26P-QCHF')
    a.add(body, name='connector', color=cq.Color(0.2, 0.2, 0.2))
    return a


MODELS = {
    'CONN-SMD_26P-P0.50_KH-FPC0.5-H1.0SMT-26P-QCHF': fpc_26p_kh,
    'Texas_DSBGA-6_0.855x1.255mm_Layout2x3_P0.4mm_LevelB': dsbga6_yff,
    'Crystal_SMD_2012-2Pin_2.0x1.2mm': crystal_2012,
    'BAT-SMD_2P-P3.00_G-Switch_GT-BTP30002-0500B-015A': bat_2p_p3_00,
    'Seeed_Wio-SX1262': wio_sx1262,
}

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '../cmd-z5.3dshapes'
    for name, fn in MODELS.items():
        path = os.path.join(out, name + '.step')
        fn().export(path)
        print('wrote', path)
