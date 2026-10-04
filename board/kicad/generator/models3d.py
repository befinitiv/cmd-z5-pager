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


MODELS = {
    'BAT-SMD_2P-P3.00_G-Switch_GT-BTP30002-0500B-015A': bat_2p_p3_00,
}

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '../cmd-z5.3dshapes'
    for name, fn in MODELS.items():
        path = os.path.join(out, name + '.step')
        fn().export(path)
        print('wrote', path)
