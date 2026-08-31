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
import route

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
    # ref: (x, y, rotation) -- position dictated by the enclosure or by what
    # has to sit next to what.
    "U1": (30.0, 13.0, 0),        # module, antenna end towards the top edge
    "U2": (38.5, 31.0, 0),        # DAC, analogue half, room for its six caps
    "U3": (14.0, 43.5, 0),        # charger, far from the analogue side
    "U4": (14.0, 29.0, 0),        # digital LDO
    "U5": (24.0, 44.0, 0),        # analogue LDO
    "J1": (4.5, 30.0, 270),       # USB-C, left edge
    "J4": (4.5, 44.0, 180),       # battery, left edge
    "J2": (52.0, 27.0, 90),       # headphone jack, right edge
    "J3": (16.0, 52.0, 90),       # OLED header, bottom edge
    "J5": (34.0, 52.0, 90),       # programming header, bottom edge
    # The three user buttons get their own row along the bottom edge, in the
    # order a thumb expects: previous, play, next. That row is why the board
    # grew 9 mm taller -- the old bottom edge was already full of headers, and
    # squeezing five switches into the sides would have put the ones people
    # press constantly behind the jack.
    "SW3": (16.0, 59.0, 0),       # BT3, previous / volume down
    "SW2": (30.0, 59.0, 0),       # BT2, play-pause / pairing
    "SW1": (44.0, 59.0, 0),       # BT1, next / volume up
    "SW4": (53.0, 41.0, 0),       # RESET, out of the way
    "SW5": (53.0, 48.5, 0),       # BOOT, out of the way
    "D1": (8.5, 51.5, 0),         # charge LED, visible at the bottom edge
    "Q1": (25.0, 26.5, 0),        # auto-reset pair, below the module
    "Q2": (29.5, 26.5, 0),
}

# Everything not anchored and not a bypass cap.
ZONES = [
    # Battery divider: left of the module, near its IO35 side.
    ("sense", 3.0, 4.0, 18.0, 21.0, ["R7", "R8", "C18"]),
    ("digital", 16.0, 19.0, 29.0, 27.5, ["R1", "R2", "C3"]),
    ("usb_cc", 10.0, 17.0, 20.0, 24.0, ["R3", "R4"]),
    ("charger", 6.0, 46.0, 22.0, 53.0, ["R5", "R6"]),
    ("dac_misc", 29.0, 40.0, 46.0, 50.0, ["R9", "JP1"]),
]


def _courtyard_box(fp):
    poly = fp.GetCourtyard(pcbnew.F_CrtYd)
    return poly.BBox() if poly.OutlineCount() else fp.GetBoundingBox(False, False)


def _courtyard_size(fp):
    """Courtyard extent in mm, falling back to the footprint bounding box."""
    box = _courtyard_box(fp)
    return pcbnew.ToMM(box.GetWidth()), pcbnew.ToMM(box.GetHeight())


def _pad_position(fp, number):
    for pad in fp.Pads():
        if pad.GetNumber() == number:
            pos = pad.GetPosition()
            return pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y)
    raise SystemExit("no pad %s on %s" % (number, fp.GetReference()))


def _boxes_overlap(a, b, gap_mm):
    inflate = pcbnew.FromMM(gap_mm)
    a = pcbnew.BOX2I(a.GetOrigin(), a.GetSize())
    a.Inflate(inflate)
    return a.Intersects(b)


def pack(sizes, loader):
    """
    Decide where everything goes.

    Order matters. Anchors first, then bypass capacitors hard against the pin
    each one serves, then the leftovers into zones. Placing the caps by
    function instead of by proximity was the first attempt, and it left C1
    twenty millimetres from the pin it decouples -- which is to say, not
    decoupling anything at all.
    """
    placement = dict(ANCHORED)
    occupied = []          # BOX2I of everything already down

    def commit(ref, x, y, rot):
        fp = loader(design.PARTS[ref][2])
        if rot:
            fp.SetOrientationDegrees(rot)
        fp.SetPosition(vec(x, y))
        box = _courtyard_box(fp)
        occupied.append(box)
        placement[ref] = (round(x, 3), round(y, 3), rot)
        return fp

    anchored_fps = {}
    for ref, (x, y, rot) in ANCHORED.items():
        anchored_fps[ref] = commit(ref, x, y, rot)

    # --- bypass caps, closest free spot to their pin ------------------------
    import math
    for cap, (ic, pin) in sorted(design.DECOUPLING.items()):
        tx, ty = _pad_position(anchored_fps[ic], pin)
        placed = False
        for radius in [x * 0.4 for x in range(5, 26)]:       # 2.0 .. 10.0 mm
            for step in range(24):
                ang = math.radians(step * 15.0)
                x = tx + radius * math.cos(ang)
                y = ty + radius * math.sin(ang)
                w, h = sizes[cap]
                if not (1.5 + w / 2 < x < design.BOARD["width_mm"] - 1.5 - w / 2):
                    continue
                if not (1.5 + h / 2 < y < design.BOARD["height_mm"] - 1.5 - h / 2):
                    continue
                fp = loader(design.PARTS[cap][2])
                fp.SetPosition(vec(x, y))
                box = _courtyard_box(fp)
                if any(_boxes_overlap(box, other, 0.25) for other in occupied):
                    continue
                commit(cap, x, y, 0)
                placed = True
                break
            if placed:
                break
        if not placed:
            raise SystemExit("nowhere to put %s near %s.%s" % (cap, ic, pin))

    # --- everything else ----------------------------------------------------
    # Scan each zone on a grid and take the first free spot, checking against
    # everything already placed. Row-packing inside a bare rectangle ignored
    # the anchored parts sitting in that rectangle, so a resistor could land
    # on top of a transistor and the zone would still report itself as full.
    for name, x0, y0, x1, y1, refs in ZONES:
        for ref in sorted(refs, key=lambda r: -sizes[r][0] * sizes[r][1]):
            w, h = sizes[ref]
            spot = None
            y = y0 + h / 2.0
            while y + h / 2.0 <= y1 and spot is None:
                x = x0 + w / 2.0
                while x + w / 2.0 <= x1:
                    fp = loader(design.PARTS[ref][2])
                    fp.SetPosition(vec(x, y))
                    box = _courtyard_box(fp)
                    if not any(_boxes_overlap(box, other, 0.25) for other in occupied):
                        spot = (x, y)
                        break
                    x += 0.5
                y += 0.5
            if spot is None:
                raise SystemExit("no free spot in zone %s for %s" % (name, ref))
            commit(ref, spot[0], spot[1], 0)

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


def add_ground_pours(board, w, h):
    """
    Ground pour on both layers, plus stitching vias.

    A pour is how GND gets connected: it is by far the largest net, a person
    would pour it rather than route it, and doing so takes roughly half the
    connections out of the routing problem. Filling happens after the signals
    exist so the zones keep clearance around them.
    """
    gnd = board.FindNet("GND")
    if gnd is None:
        return

    margin = 0.3
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        zone = pcbnew.ZONE(board)
        zone.SetLayer(layer)
        zone.SetNet(gnd)
        zone.SetAssignedPriority(0)
        zone.SetLocalClearance(pcbnew.FromMM(0.25))
        zone.SetMinThickness(pcbnew.FromMM(0.2))
        # Solid, not thermal relief. Thermal spokes on 0805 pads come out
        # thinner than the zone's minimum thickness and DRC calls them starved;
        # a hand-soldered board of this size does not need the relief anyway.
        zone.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)

        outline = zone.Outline()
        outline.NewOutline()
        for x, y in ((margin, margin), (w - margin, margin),
                     (w - margin, h - margin), (margin, h - margin)):
            outline.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
        board.Add(zone)

    # Stitching vias tie the two pours together. Placed on a coarse grid and
    # skipped wherever anything already sits, so they never collide.
    for gx in range(1, int(w / 7.0) + 1):
        for gy in range(1, int(h / 7.0) + 1):
            x, y = gx * 7.0, gy * 7.0
            spot = pcbnew.VECTOR2I(mm(x), mm(y))
            clash = False
            for fp in board.GetFootprints():
                box = _courtyard_box(fp)
                box.Inflate(mm(0.8))
                if box.Contains(spot):
                    clash = True
                    break
            if clash:
                continue
            # Keep well clear of anything already in copper. Hole-to-hole is
            # its own DRC rule, so a stitching via landing near a through-hole
            # pad is an error even when the copper would have been fine.
            for track in board.GetTracks():
                if track.HitTest(spot, pcbnew.FromMM(1.2)):
                    clash = True
                    break
            if not clash:
                for fp2 in board.GetFootprints():
                    for pad in fp2.Pads():
                        if pad.HasHole() and (pad.GetPosition() - spot).EuclideanNorm() < mm(1.5):
                            clash = True
                            break
                    if clash:
                        break
            if clash:
                continue
            via = pcbnew.PCB_VIA(board)
            via.SetPosition(spot)
            via.SetWidth(mm(route.VIA_DIA_MM))
            via.SetDrill(mm(route.VIA_DRILL_MM))
            via.SetNet(gnd)
            board.Add(via)

    pcbnew.ZONE_FILLER(board).Fill(board.Zones())


def build():
    board = pcbnew.BOARD()

    # A bare BOARD() carries defaults that reject perfectly normal footprints:
    # the WROOM module's thermal vias drill 0.2 mm, under the 0.3 mm default
    # minimum. Set rules that match what a cheap two-layer fab actually offers.
    # JLCPCB's standard two-layer capability, with margin. Their published
    # floors are 0.127 mm track and space, 0.2 mm drill, 0.13 mm annular ring
    # and 0.5 mm hole-to-hole; these sit at or inside those, so the board is
    # buildable on their cheapest process. The same numbers are in the project
    # file's netclass, which is what DRC actually enforces -- the values here
    # are the board's own floor.
    settings = board.GetDesignSettings()
    settings.m_MinClearance = mm(0.15)
    settings.m_TrackMinWidth = mm(0.15)
    settings.m_MinThroughDrill = mm(0.2)
    settings.m_ViasMinSize = mm(0.5)
    settings.m_ViasMinAnnularWidth = mm(0.13)
    settings.m_HoleToHoleMin = mm(0.5)
    settings.m_CopperEdgeClearance = mm(0.5)

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
    placement = pack(sizes, load_footprint)

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

    # --- copper: route the signals, then pour ground around them -----------
    routed, failed = route.route_board(board, w, h)
    print("  routed %d nets" % len(routed))
    if failed:
        print("  FAILED to route: %s" % ", ".join(sorted(failed)))

    add_ground_pours(board, w, h)

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
    tracks = sum(1 for t in board.GetTracks() if isinstance(t, pcbnew.PCB_TRACK)
                 and not isinstance(t, pcbnew.PCB_VIA))
    vias = sum(1 for t in board.GetTracks() if isinstance(t, pcbnew.PCB_VIA))
    print("  board %.0f x %.0f mm, %d tracks, %d vias, %d zones"
          % (design.BOARD["width_mm"], design.BOARD["height_mm"],
             tracks, vias, len(board.Zones())))
