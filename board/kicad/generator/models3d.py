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


MODELS = {
    'BAT-SMD_2P-P3.00_G-Switch_GT-BTP30002-0500B-015A': bat_2p_p3_00,
    'Seeed_Wio-SX1262': wio_sx1262,
}

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '../cmd-z5.3dshapes'
    for name, fn in MODELS.items():
        path = os.path.join(out, name + '.step')
        fn().export(path)
        print('wrote', path)
