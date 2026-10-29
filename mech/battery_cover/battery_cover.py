"""
Battery cover for the Sony CMD-Z5, replacing the shell of the original
battery pack (the cell is now wired to J8 and sits in the compartment).
FDM-printable, printed outer face down.

Coordinates: x along the cover, 0 at the contact end (tabs), LENGTH at the
latch end; y across, centred; z = 0 is the outer face (on the print bed),
z = THICKNESS the inner face.

Measured on the original shell (see ../../hw/): 46.5 x 64.5 mm; two tabs at
the contact end, 16.5 mm apart (inner edges), 5.6 mm wide, 1 mm protrusion;
latch 18 mm wide, 1.4 mm high, 6 mm spring tongue. Compartment at the latch
end 6.3 mm deep, the shell sits 2.5 mm deep there; rim to the contact blocks
2.7 mm. The corner radius is estimated from the 300 dpi scan.

    python battery_cover.py          # writes out/*.stl and out/*.step

The latch is not modelled yet (how it engages the housing is still open).
"""

from pathlib import Path

from build123d import (
    Align,
    Axis,
    Box,
    BuildPart,
    BuildSketch,
    Locations,
    Mode,
    RectangleRounded,
    chamfer,
    export_step,
    export_stl,
    extrude,
)

# Outline of the original shell and the fit clearance per side.
LENGTH_NOMINAL = 64.5
WIDTH_NOMINAL = 46.5
CLEARANCE = 0.15
CORNER_RADIUS = 4.0
THICKNESS = 2.0
EDGE_CHAMFER = 0.6  # outer face edge, 45 deg (prints as a 45 deg overhang)

# Tabs at the contact end: they go under the housing rim.
TAB_GAP = 16.5  # between the inner edges
TAB_WIDTH = 5.6
TAB_PROTRUSION = 1.0
TAB_THICKNESS = 1.0  # at the inner face

# Test print of the outline only.
FIT_TEST_THICKNESS = 0.6

LENGTH = LENGTH_NOMINAL - 2 * CLEARANCE
WIDTH = WIDTH_NOMINAL - 2 * CLEARANCE


def tabs(thickness):
    """The two tabs, at the inner face of a plate of this thickness."""
    z0 = thickness - TAB_THICKNESS
    for side in (-1, 1):
        y = side * (TAB_GAP / 2 + TAB_WIDTH / 2)
        with Locations((-TAB_PROTRUSION / 2 + 0.5, y, z0 + TAB_THICKNESS / 2)):
            Box(TAB_PROTRUSION + 1.0, TAB_WIDTH, TAB_THICKNESS)


def cover(thickness=THICKNESS, edge_chamfer=EDGE_CHAMFER):
    with BuildPart() as part:
        with BuildSketch():
            RectangleRounded(LENGTH, WIDTH, CORNER_RADIUS, align=(Align.MIN, Align.CENTER))
        extrude(amount=thickness)
        if edge_chamfer:
            chamfer(part.edges().group_by(Axis.Z)[0], edge_chamfer)
        tabs(thickness)
    return part.part


def main():
    out = Path(__file__).parent / "out"
    out.mkdir(exist_ok=True)
    full = cover()
    fit = cover(FIT_TEST_THICKNESS, 0)
    export_stl(full, str(out / "battery_cover.stl"))
    export_step(full, str(out / "battery_cover.step"))
    export_stl(fit, str(out / "battery_cover_fit_test.stl"))
    bb = full.bounding_box()
    print(f"cover {bb.size.X:.2f} x {bb.size.Y:.2f} x {bb.size.Z:.2f} mm, "
          f"volume {full.volume:.0f} mm3")


if __name__ == "__main__":
    main()
