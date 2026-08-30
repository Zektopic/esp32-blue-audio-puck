"""
Emit hardware/blueaudio-puck.kicad_sch from design.py.

Connectivity is by net label rather than by drawn wire: every pin gets a short
stub and a label carrying its net name. That is a normal style for a generated
or dense schematic, it is exactly what ERC checks, and it avoids inventing a
routing aesthetic that a human would want to redo anyway. Open it in Eeschema
and drag things around -- the netlist survives.

Schematic Y grows downward while symbol Y grows upward, so every pin coordinate
is flipped on the way in. That single sign is the difference between a sheet
that connects and one that silently does not.
"""

import io
import os
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import design
import kisym

SCH_VERSION = "20250610"
GENERATOR_VERSION = "9.99"
PROJECT = "blueaudio-puck"

STUB = 3.81          # mm of wire between a pin and its label
GRID = 1.27

ROOT_UUID = "00000000-0000-4000-8000-000000000001"


def uid(seed):
    """Stable UUIDs, so regenerating does not churn the whole file in git."""
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "blueaudio-puck/" + seed))


def snap(v):
    """
    Round for tidiness only -- never to a grid.

    Connectivity in KiCad is by exact coordinate. Snapping a pin endpoint to
    the 1.27 mm grid moves the wire off the pin it was supposed to meet, and
    the result is a schematic that looks connected and reports several hundred
    dangling labels.
    """
    return round(v, 4)


# ---------------------------------------------------------------------------
# Sheet layout. Positions are the symbol origins, in mm on an A3 sheet.
# ---------------------------------------------------------------------------

def _extent(lib, sym, stub):
    """Bounding box a placed symbol needs, stubs and labels included."""
    xs, ys = [0.0], [0.0]
    for pin in kisym.pins(lib, sym):
        cx, cy = pin["connect"]
        odx, ody = pin["outward"]
        xs += [cx, cx + odx * stub]
        ys += [-cy, -cy - ody * stub]
    # Labels run outward from the stub end; leave room for the longest one.
    pad = 22.0
    return (min(xs) - pad, min(ys) - 10.0, max(xs) + pad, max(ys) + 10.0)


def auto_place(stub):
    """
    Lay parts out in rows, largest first, with enough clearance that no two
    stubs can reach each other.

    Hand-tuned coordinates were how two collinear stubs ended up overlapping by
    0.24 mm and shorting IO0 to VBAT. Deriving the spacing from each symbol's
    actual extent removes that whole class of mistake, at the cost of a sheet
    nobody would call pretty. Eeschema can rearrange it; the netlist will hold.
    """
    boxes = {}
    for ref, (lib, sym, _fp, _val, _d) in design.PARTS.items():
        boxes[ref] = _extent(lib, sym, stub)

    def area(ref):
        x0, y0, x1, y1 = boxes[ref]
        return (x1 - x0) * (y1 - y0)

    order = sorted(design.PARTS, key=lambda r: (-area(r), r))

    placement = {}
    margin = 12.0
    cursor_x, cursor_y, row_h = margin, margin, 0.0
    sheet_w = 580.0                       # A2 landscape, minus a border

    for ref in order:
        x0, y0, x1, y1 = boxes[ref]
        w, h = (x1 - x0), (y1 - y0)
        if cursor_x + w > sheet_w and row_h > 0:
            cursor_x = margin
            cursor_y += row_h + margin
            row_h = 0.0
        # Snap the origin to the 1.27 mm grid. Pin offsets inside a symbol are
        # almost always grid multiples, so an on-grid origin puts the pins and
        # their stubs on grid too, and KiCad stops warning about every endpoint.
        gx = round((cursor_x - x0) / 1.27) * 1.27
        gy = round((cursor_y - y0) / 1.27) * 1.27
        placement[ref] = (round(gx, 4), round(gy, 4))
        cursor_x += w + margin
        row_h = max(row_h, h)

    return placement, cursor_y + row_h + margin


PLACEMENT, SHEET_USED_MM = auto_place(STUB)

# A power flag tells ERC a net is driven. Only nets whose every pin is an input
# or passive need one: putting a flag on a rail that already has a regulator
# output is two power outputs on one net, which ERC calls an error -- correctly.
POWER_FLAGS = {}
_flag_x = 600.0
for _net in ("GND", "VBUS"):
    POWER_FLAGS[_net] = (round(_flag_x / 1.27) * 1.27,
                         round((40.0 + 30.0 * len(POWER_FLAGS)) / 1.27) * 1.27)


def sym_props(ref, value, footprint, x, y, hide_fp=True):
    """Reference above the part, value below, everything else hidden."""
    def prop(name, val, dy, hide):
        return (
            '\t\t(property "%s" "%s"\n'
            '\t\t\t(at %s %s 0)\n'
            '%s'
            '\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n\t\t\t)\n'
            '\t\t)\n' % (name, val, x, round(y + dy, 4),
                         '\t\t\t(hide yes)\n' if hide else '')
        )

    out = prop("Reference", ref, -8.89, False)
    out += prop("Value", value, -6.35, False)
    out += prop("Footprint", footprint or "", 0, hide_fp)
    out += prop("Datasheet", "", 2.54, True)
    out += prop("Description", "", 5.08, True)
    return out


def emit_symbol(ref, lib_id, value, footprint, x, y):
    return (
        '\t(symbol\n'
        '\t\t(lib_id "%s")\n'
        '\t\t(at %s %s 0)\n'
        '\t\t(unit 1)\n'
        '\t\t(exclude_from_sim no)\n'
        '\t\t(in_bom yes)\n'
        '\t\t(on_board yes)\n'
        '\t\t(dnp no)\n'
        '\t\t(uuid "%s")\n'
        '%s'
        '\t\t(instances\n'
        '\t\t\t(project "%s"\n'
        '\t\t\t\t(path "/%s"\n'
        '\t\t\t\t\t(reference "%s")\n'
        '\t\t\t\t\t(unit 1)\n'
        '\t\t\t\t)\n'
        '\t\t\t)\n'
        '\t\t)\n'
        '\t)\n' % (lib_id, x, y, uid("sym/" + ref),
                   sym_props(ref, value, footprint, x, y),
                   PROJECT, ROOT_UUID, ref)
    )


def emit_wire(x1, y1, x2, y2, seed):
    return (
        '\t(wire\n\t\t(pts\n\t\t\t(xy %s %s) (xy %s %s)\n\t\t)\n'
        '\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type default)\n\t\t)\n'
        '\t\t(uuid "%s")\n\t)\n' % (x1, y1, x2, y2, uid("wire/" + seed))
    )


def emit_label(name, x, y, angle, justify, seed):
    return (
        '\t(label "%s"\n\t\t(at %s %s %d)\n'
        '\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1.27 1.27)\n\t\t\t)\n'
        '\t\t\t(justify %s bottom)\n\t\t)\n'
        '\t\t(uuid "%s")\n\t)\n' % (name, x, y, angle, justify, uid("lbl/" + seed))
    )


def emit_no_connect(x, y, seed):
    return '\t(no_connect\n\t\t(at %s %s)\n\t\t(uuid "%s")\n\t)\n' % (
        x, y, uid("nc/" + seed))


def build():
    mapping = design.pin_to_net()
    nc = set(design.NO_CONNECT)

    # --- symbols to embed, including the power flag ------------------------
    used = {}
    for ref, (lib, sym, _fp, _val, _d) in design.PARTS.items():
        used["%s:%s" % (lib, sym)] = (lib, sym)
    used["power:PWR_FLAG"] = ("power", "PWR_FLAG")

    lib_block = "\t(lib_symbols\n"
    for lib_id in sorted(used):
        lib, sym = used[lib_id]
        block = kisym.resolve(lib, sym)
        # lib_symbols entries are keyed by "Lib:Name", not the bare name.
        block = block.replace('(symbol "%s"' % sym, '(symbol "%s"' % lib_id, 1)
        lib_block += "\n".join("\t\t" + ln.lstrip("\t")
                               for ln in block.splitlines()) + "\n"
    lib_block += "\t)\n"

    body = ""
    stats = {"labels": 0, "nc": 0}

    for ref in sorted(design.PARTS):
        lib, sym, fp, val, _desc = design.PARTS[ref]
        ox, oy = PLACEMENT[ref]
        body += emit_symbol(ref, "%s:%s" % (lib, sym), val, fp, ox, oy)

        for pin in kisym.pins(lib, sym):
            cx, cy = pin["connect"]
            # Symbol Y grows upward, sheet Y grows downward.
            px, py = snap(ox + cx), snap(oy - cy)
            odx, ody = pin["outward"]
            dx, dy = odx, -ody      # same flip applied to the direction

            key = (ref, pin["number"])
            seed = "%s.%s" % (ref, pin["number"])

            if key in nc:
                body += emit_no_connect(px, py, seed)
                stats["nc"] += 1
                continue

            net = mapping.get(key)
            if net is None:
                continue     # design.check() has already reported this

            lx, ly = snap(px + dx * STUB), snap(py + dy * STUB)
            body += emit_wire(px, py, lx, ly, seed)

            if dx > 0:
                angle, justify = 0, "left"
            elif dx < 0:
                angle, justify = 180, "right"
            elif dy < 0:
                angle, justify = 90, "left"
            else:
                angle, justify = 270, "left"
            body += emit_label(net, lx, ly, angle, justify, seed)
            stats["labels"] += 1

    # --- power flags -------------------------------------------------------
    for i, (net, (fx, fy)) in enumerate(sorted(POWER_FLAGS.items())):
        ref = "#FLG%02d" % (i + 1)
        body += emit_symbol(ref, "power:PWR_FLAG", "PWR_FLAG", "", fx, fy)
        pin = kisym.pins("power", "PWR_FLAG")[0]
        cx, cy = pin["connect"]
        px, py = snap(fx + cx), snap(fy - cy)
        ly = snap(py - pin["outward"][1] * STUB)
        body += emit_wire(px, py, px, ly, "flag/" + net)
        body += emit_label(net, px, ly, 90, "left", "flag/" + net)
        stats["labels"] += 1

    sheet = (
        '(kicad_sch\n'
        '\t(version %s)\n'
        '\t(generator "eeschema")\n'
        '\t(generator_version "%s")\n'
        '\t(uuid "%s")\n'
        '\t(paper "A2")\n'
        '\t(title_block\n'
        '\t\t(title "BlueAudio Puck carrier, rev A")\n'
        '\t\t(rev "A")\n'
        '\t\t(company "Zektopic")\n'
        '\t\t(comment 1 "Generated by hardware/scripts/gen_schematic.py -- edit design.py, not this file")\n'
        '\t\t(comment 2 "Connectivity is by net label; drag symbols freely, the netlist follows")\n'
        '\t)\n'
        '%s'
        '%s'
        '\t(sheet_instances\n\t\t(path "/"\n\t\t\t(page "1")\n\t\t)\n\t)\n'
        '\t(embedded_fonts no)\n'
        ')\n' % (SCH_VERSION, GENERATOR_VERSION, ROOT_UUID, lib_block, body)
    )
    return sheet, stats


if __name__ == "__main__":
    issues = design.check()
    if issues:
        for p in issues:
            print("design error:", p)
        raise SystemExit(1)

    text, stats = build()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                       "blueaudio-puck.kicad_sch")
    out = os.path.normpath(out)
    io.open(out, "w", encoding="utf-8", newline="\n").write(text)
    print("wrote %s (%d bytes)" % (out, len(text)))
    print("  %d symbols, %d labels, %d no-connects"
          % (len(design.PARTS), stats["labels"], stats["nc"]))
