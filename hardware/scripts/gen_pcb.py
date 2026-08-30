"""
Emit hardware/blueaudio-puck.kicad_pcb from design.py, using KiCad's own API.

Run with KiCad's bundled Python, which is the only one that has pcbnew:

    "C:\\Users\\manup\\AppData\\Local\\Programs\\KiCad\\10.0\\bin\\python.exe" gen_pcb.py

What this produces: every footprint on the board, every pad assigned to the
correct net, a rounded board outline with mounting holes, and a sensible
starting placement.

What it does NOT produce: routing. There are no tracks and no copper pours.
That is deliberate rather than unfinished -- a half-routed board looks done and
is worse than an obviously unrouted one, and the analogue section here wants
decisions (star ground, keeping the DAC's return away from the radio) that are
judgement rather than arithmetic. Open it in Pcbnew and the ratsnest will show
exactly what needs connecting.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import design

try:
    import pcbnew
except ImportError:                                        # pragma: no cover
    raise SystemExit("run this with KiCad's bundled python.exe -- pcbnew is not "
                     "importable from a normal interpreter")

FOOTPRINT_DIR = os.path.join(
    r"C:\Users\manup\AppData\Local\Programs\KiCad\10.0\share\kicad", "footprints")

OUT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "blueaudio-puck.kicad_pcb"))


def mm(v):
    return pcbnew.FromMM(float(v))


def vec(x, y):
    return pcbnew.VECTOR2I(mm(x), mm(y))


# ---------------------------------------------------------------------------
# Placement. Rough functional zones, so the starting point is not nonsense:
# radio at the top with its antenna edge clear, analogue on the opposite side
# from the switching and charging, connectors on the edges they exit from.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Placement.
#
# The parts whose position is decided by the enclosure -- connectors on the
# edges they exit from, the module with its antenna end clear -- are anchored
# by hand. Everything else is packed automatically into the space that is left,
# using each footprint's real courtyard.
#
# Hand-placing forty parts on a grid is how pads end up overlapping: the first
# attempt shorted EN to GND and VBUS to EN, because a courtyard is bigger than
# the part you picture. Packing from the actual courtyards cannot make that
# mistake, and pack() asserts it afterwards.
# ---------------------------------------------------------------------------

ANCHORED = {
    # ref: (x, y, rotation) -- position dictated by the enclosure
    "U1": (30.0, 13.0, 0),        # module, antenna end towards the top edge
    "J1": (4.5, 30.0, 270),       # USB-C, left edge
    "J4": (4.5, 44.0, 180),       # battery connector, left edge
    "J2": (52.0, 27.0, 90),       # headphone jack, right edge
    # Headers lie along the bottom edge; unrotated they would run down the
    # board and hang off it.
    "J3": (16.0, 52.0, 90),       # OLED header
    "J5": (34.0, 52.0, 90),       # programming header
    "SW1": (53.0, 48.5, 0),      # buttons, reachable corner
    "SW2": (52.0, 40.0, 0),
    "D1": (10.0, 51.0, 0),        # charge LED, visible at the bottom edge
}

# Where the packer may put everything else, and roughly what belongs together.
# Analogue parts are kept on the opposite side of the board from the charger.
ZONES = [
    # (name, x0, y0, x1, y1, [refs])
    ("analogue", 31.0, 25.0, 45.5, 44.0,
     ["U2", "U5", "C5", "C6", "C7", "C8", "C9", "C10", "C16", "C17", "C15",
      "C4", "R9"]),
    ("digital", 31.0, 44.5, 47.0, 50.0,
     ["R7", "R8", "C18", "R1", "R2", "JP1"]),
    ("power", 12.0, 24.0, 30.0, 49.0,
     ["U3", "U4", "C1", "C2", "C3", "C11", "C12", "C13", "C14",
      "R3", "R4", "R5", "R6"]),
]


def _courtyard_size(fp):
    """Courtyard extent in mm, falling back to the footprint bounding box."""
    poly = fp.GetCourtyard(pcbnew.F_CrtYd)
    box = poly.BBox() if poly.OutlineCount() else fp.GetBoundingBox(False, False)
    return pcbnew.ToMM(box.GetWidth()), pcbnew.ToMM(box.GetHeight())


def pack(sizes):
    """
    Place the unanchored parts inside their zones, largest first.

    Returns {ref: (x, y, rotation)}. Raises if a zone cannot hold its parts --
    silently spilling them on top of each other is exactly the failure this
    exists to prevent.
    """
    gap = 0.6
    placement = dict(ANCHORED)

    for name, x0, y0, x1, y1, refs in ZONES:
        todo = sorted(refs, key=lambda r: -sizes[r][1])
        cx, cy, row_h = x0, y0, 0.0
        for ref in todo:
            w, h = sizes[ref]
            if cx + w > x1 and row_h > 0:
                cx = x0
                cy += row_h + gap
                row_h = 0.0
            if cy + h > y1:
                raise SystemExit("zone %s is too small for %s -- enlarge it or "
                                 "grow the board" % (name, ref))
            placement[ref] = (round(cx + w / 2.0, 3), round(cy + h / 2.0, 3), 0)
            cx += w + gap
            row_h = max(row_h, h)

    missing = sorted(set(design.PARTS) - set(placement))
    if missing:
        raise SystemExit("no placement for %s" % missing)
    return placement


def assert_no_overlap(board):
    """Courtyards must not intersect. Overlapping courtyards mean shorts."""
    boxes = []
    for fp in board.GetFootprints():
        poly = fp.GetCourtyard(pcbnew.F_CrtYd)
        if not poly.OutlineCount():
            continue
        boxes.append((fp.GetReference(), poly.BBox()))

    clashes = []
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            a, b = boxes[i][1], boxes[j][1]
            if a.Intersects(b):
                clashes.append("%s and %s" % (boxes[i][0], boxes[j][0]))
    return clashes


def load_footprint(spec):
    """spec is "Library:Footprint"."""
    lib, name = spec.split(":", 1)
    path = os.path.join(FOOTPRINT_DIR, lib + ".pretty")
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None:
        raise SystemExit("footprint not found: %s" % spec)
    return fp


def build():
    board = pcbnew.BOARD()

    # A bare BOARD() carries defaults that reject perfectly normal footprints:
    # the WROOM module's thermal vias drill 0.2 mm, under the 0.3 mm default
    # minimum. Set rules that match what a cheap two-layer fab actually offers.
    settings = board.GetDesignSettings()
    settings.m_MinClearance = mm(0.15)
    settings.m_TrackMinWidth = mm(0.2)
    settings.m_MinThroughDrill = mm(0.2)
    settings.m_ViasMinSize = mm(0.45)
    settings.m_ViasMinAnnularWidth = mm(0.1)

    # --- nets ---------------------------------------------------------------
    nets = {}
    for name in sorted(design.NETS):
        item = pcbnew.NETINFO_ITEM(board, name)
        board.Add(item)
        nets[name] = item

    mapping = design.pin_to_net()

    # --- measure every footprint before deciding where anything goes --------
    sizes = {}
    for ref, (_lib, _sym, fp_spec, _val, _d) in design.PARTS.items():
        sizes[ref] = _courtyard_size(load_footprint(fp_spec))
    placement = pack(sizes)

    # --- footprints ---------------------------------------------------------
    missing_pads = []
    for ref in sorted(design.PARTS):
        lib, sym, fp_spec, value, _desc = design.PARTS[ref]
        fp = load_footprint(fp_spec)
        fp.SetReference(ref)
        fp.SetValue(value)

        x, y, rot = placement[ref]
        fp.SetPosition(vec(x, y))
        if rot:
            fp.SetOrientationDegrees(rot)
        board.Add(fp)

        # Pad numbers in a footprint match pin numbers in the symbol, which is
        # the whole basis of the schematic-to-board correspondence.
        for pad in fp.Pads():
            number = pad.GetNumber()
            net = mapping.get((ref, number))
            if net is not None:
                pad.SetNet(nets[net])
            elif number == "" and ref in design.MECHANICAL_TO_GND:
                pad.SetNet(nets["GND"])
            elif (ref, number) not in set(design.NO_CONNECT):
                missing_pads.append("%s pad %r" % (ref, number))

    # --- board outline ------------------------------------------------------
    # A plain rectangle. Rounded corners were the first attempt and produced
    # ten "malformed outline" errors: an arc whose endpoints do not land
    # exactly on its neighbours leaves the outline open, and KiCad will not
    # guess. Four segments close by construction. Round the corners in Pcbnew
    # once the board is routed, where the editor keeps the ends joined.
    w = design.BOARD["width_mm"]
    h = design.BOARD["height_mm"]

    for (x1, y1), (x2, y2) in [((0, 0), (w, 0)), ((w, 0), (w, h)),
                               ((w, h), (0, h)), ((0, h), (0, 0))]:
        seg = pcbnew.PCB_SHAPE(board)
        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetStart(vec(x1, y1))
        seg.SetEnd(vec(x2, y2))
        seg.SetLayer(pcbnew.Edge_Cuts)
        seg.SetWidth(mm(0.1))
        board.Add(seg)

    # --- mounting holes -----------------------------------------------------
    inset = design.BOARD["mounting_inset_mm"]
    dia = design.BOARD["mounting_hole_dia_mm"]
    for i, (hx, hy) in enumerate([(inset, inset), (w - inset, inset),
                                  (inset, h - inset), (w - inset, h - inset)]):
        hole = pcbnew.FOOTPRINT(board)
        pad = pcbnew.PAD(hole)
        pad.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
        pad.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
        pad.SetSize(pcbnew.VECTOR2I(mm(dia), mm(dia)))
        pad.SetDrillSize(pcbnew.VECTOR2I(mm(dia), mm(dia)))
        pad.SetPosition(vec(hx, hy))
        pad.SetLayerSet(pad.UnplatedHoleMask())
        hole.Add(pad)
        hole.SetReference("H%d" % (i + 1))
        hole.SetPosition(vec(hx, hy))
        board.Add(hole)

    clashes = assert_no_overlap(board)
    if clashes:
        raise SystemExit("courtyards overlap: %s" % "; ".join(clashes[:10]))

    return board, missing_pads


if __name__ == "__main__":
    issues = design.check()
    if issues:
        for p in issues:
            print("design error:", p)
        raise SystemExit(1)

    board, missing = build()

    if missing:
        print("pads with no net and no explicit no-connect:")
        for m in missing:
            print("  -", m)

    pcbnew.SaveBoard(OUT, board)
    print("wrote %s" % OUT)
    print("  %d footprints, %d nets" % (len(design.PARTS), len(design.NETS)))
    print("  board %.0f x %.0f mm, no tracks (routing is left to a human)"
          % (design.BOARD["width_mm"], design.BOARD["height_mm"]))
