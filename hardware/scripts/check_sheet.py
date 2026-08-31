"""
Look for stubs from different nets that touch each other.

Label-based connectivity is only safe if no two stubs share any geometry. When
they do, KiCad silently merges the nets and reports it as `multiple_net_names`
somewhere far from the cause -- IO0 shorted to VBAT, in one memorable case
where two collinear stubs overlapped by 0.24 mm.

That 0.24 mm is why this checks segments properly instead of sampling points
along them: a sampler steps past a sub-step overlap and reports all clear.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import design
import kisym

EPS = 1e-6


def stub_geometry(placement, stub_len):
    """[(net, p1, p2, who)] for every stub the generator would draw."""
    mapping = design.pin_to_net()
    nc = set(design.NO_CONNECT)
    out = []

    for ref, (lib, sym, _fp, _val, _d) in design.PARTS.items():
        ox, oy = placement[ref]
        for pin in kisym.pins(lib, sym):
            key = (ref, pin["number"])
            net = mapping.get(key)
            cx, cy = pin["connect"]
            px, py = round(ox + cx, 4), round(oy - cy, 4)
            odx, ody = pin["outward"]
            dx, dy = odx, -ody
            lx, ly = round(px + dx * stub_len, 4), round(py + dy * stub_len, 4)
            label = "%s.%s" % (ref, pin["number"])
            if key in nc:
                # A no-connect still occupies its pin point.
                out.append(("<nc>" + label, (px, py), (px, py), label))
            elif net is not None:
                out.append((net, (px, py), (lx, ly), label))
    return out


def _overlap(a, b):
    """True if two axis-aligned segments share more than a single crossing."""
    (ax1, ay1), (ax2, ay2) = a
    (bx1, by1), (bx2, by2) = b

    a_vert = abs(ax1 - ax2) < EPS
    b_vert = abs(bx1 - bx2) < EPS

    if a_vert and b_vert and abs(ax1 - bx1) < EPS:
        lo1, hi1 = sorted((ay1, ay2))
        lo2, hi2 = sorted((by1, by2))
        return min(hi1, hi2) - max(lo1, lo2) > -EPS
    if (not a_vert) and (not b_vert) and abs(ay1 - by1) < EPS:
        lo1, hi1 = sorted((ax1, ax2))
        lo2, hi2 = sorted((bx1, bx2))
        return min(hi1, hi2) - max(lo1, lo2) > -EPS

    # Perpendicular, or a degenerate point: a shared point still connects.
    for p in (a[0], a[1]):
        if _point_on(p, b):
            return True
    for p in (b[0], b[1]):
        if _point_on(p, a):
            return True
    return False


def _point_on(p, seg):
    (x, y) = p
    (x1, y1), (x2, y2) = seg
    if min(x1, x2) - EPS <= x <= max(x1, x2) + EPS and \
       min(y1, y2) - EPS <= y <= max(y1, y2) + EPS:
        # Collinear check for axis-aligned segments.
        if abs(x1 - x2) < EPS:
            return abs(x - x1) < EPS
        if abs(y1 - y2) < EPS:
            return abs(y - y1) < EPS
    return False


def collisions(placement, stub_len):
    segs = stub_geometry(placement, stub_len)
    problems = []
    for i in range(len(segs)):
        net_a, a1, a2, who_a = segs[i]
        for j in range(i + 1, len(segs)):
            net_b, b1, b2, who_b = segs[j]
            if net_a == net_b:
                continue
            if _overlap((a1, a2), (b1, b2)):
                problems.append("%s (%s) touches %s (%s)"
                                % (net_a, who_a, net_b, who_b))
    return sorted(set(problems))


if __name__ == "__main__":
    import gen_schematic

    bad = collisions(gen_schematic.PLACEMENT, gen_schematic.STUB)
    if bad:
        print("%d stub collisions:" % len(bad))
        for b in bad[:40]:
            print("  -", b)
        raise SystemExit(1)
    print("no stub collisions")
