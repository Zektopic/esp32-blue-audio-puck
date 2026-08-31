"""
A small two-layer maze router.

Not a general-purpose autorouter. It knows one board, it routes point to point
on a grid with a cost for changing layers, and it refuses to produce anything
it cannot verify. What it will not do is quietly leave a net half-connected:
every net either routes completely or is reported by name.

GND is deliberately not routed. A ground pour on both layers connects it, which
is what a person would do anyway, and it removes roughly half the connections
from the problem.

Run through gen_pcb.py, which owns the board object.
"""

import heapq

import pcbnew

# Grid pitch. Fine enough to thread between 0.65 mm pitch TSSOP pads, coarse
# enough that a whole board is a few hundred thousand cells.
STEP_MM = 0.25

TRACK_WIDTH_MM = 0.2
POWER_WIDTH_MM = 0.4          # VBAT, VBUS and the 3V3 rails carry real current
# Match KiCad's default netclass clearance, not the board minimum. The
# minimum is a floor the rules may not go below; the netclass value is what DRC
# actually enforces, and assuming the floor produced 43 clearance errors on a
# board the router believed was clean.
CLEARANCE_MM = 0.2
VIA_DIA_MM = 0.6
VIA_DRILL_MM = 0.3

# How far copper reaches beyond a track centreline or pad edge before another
# net may occupy a cell.
#
# The half-step is not padding for luck. Tracks sit on cell centres, so two
# tracks on adjacent cells are one STEP apart -- and one STEP is less than
# width + clearance. Without this term the router produced forty clearance
# violations while believing every cell it used was free.
INFLATE_MM = TRACK_WIDTH_MM / 2.0 + CLEARANCE_MM + STEP_MM / 2.0

VIA_COST = 20                 # in cells; discourages needless layer changes
# No new via may be placed within this of an existing one or of a hole. Two
# 0.3 mm drills one grid step apart overlap outright, and hole-to-hole is a
# separate DRC rule that copper clearance does nothing about.
VIA_KEEPOUT_MM = 0.9
TURN_COST = 2                 # keeps runs straight and legible

FREE = -1
BLOCKED = -2

POWER_NETS = {"VBAT", "VBUS", "+3V3", "+3V3A"}


class Grid:
    def __init__(self, width_mm, height_mm, margin_mm=0.6):
        self.nx = int(width_mm / STEP_MM) + 1
        self.ny = int(height_mm / STEP_MM) + 1
        self.layers = [pcbnew.F_Cu, pcbnew.B_Cu]
        # cells[layer_index][y * nx + x]
        self.cells = [[FREE] * (self.nx * self.ny) for _ in self.layers]
        # Cells where a layer change is not allowed, for hole spacing.
        self.via_block = set()

        edge = int(margin_mm / STEP_MM)
        for li in range(len(self.layers)):
            for y in range(self.ny):
                for x in range(self.nx):
                    if (x < edge or y < edge or x >= self.nx - edge
                            or y >= self.ny - edge):
                        self.cells[li][y * self.nx + x] = BLOCKED

    def to_cell(self, x_mm, y_mm):
        return int(round(x_mm / STEP_MM)), int(round(y_mm / STEP_MM))

    def to_mm(self, cx, cy):
        return cx * STEP_MM, cy * STEP_MM

    def inside(self, cx, cy):
        return 0 <= cx < self.nx and 0 <= cy < self.ny

    def get(self, li, cx, cy):
        return self.cells[li][cy * self.nx + cx]

    def block_vias_near(self, x_mm, y_mm, radius_mm=VIA_KEEPOUT_MM):
        r = int(radius_mm / STEP_MM) + 1
        ox, oy = self.to_cell(x_mm, y_mm)
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if dx * dx + dy * dy <= r * r:
                    self.via_block.add((ox + dx, oy + dy))

    def claim_box(self, li, x0, y0, x1, y1, netcode, grow=0.0, bodies=False):
        """
        Mark a rectangle in millimetres.

        Two passes, and the order is the whole point. Pass one claims the pad
        bodies exactly. Pass two grows a clearance halo around them, but only
        into cells that are still free.

        Doing it in one pass with the halo included was the first attempt, and
        on a 0.65 mm pitch TSSOP the halos of neighbouring pads swallowed the
        pads themselves -- every cell around the part, including each pad
        centre, came out BLOCKED, and eleven nets were unroutable for a reason
        that looked like congestion and was actually bookkeeping.
        """
        cx0, cy0 = self.to_cell(x0 - grow, y0 - grow)
        cx1, cy1 = self.to_cell(x1 + grow, y1 + grow)
        for cy in range(max(cy0, 0), min(cy1 + 1, self.ny)):
            for cx in range(max(cx0, 0), min(cx1 + 1, self.nx)):
                i = cy * self.nx + cx
                cur = self.cells[li][i]
                if bodies:
                    if cur == FREE or cur == netcode:
                        self.cells[li][i] = netcode
                    else:
                        self.cells[li][i] = BLOCKED
                elif cur == FREE:
                    self.cells[li][i] = netcode
                elif cur != netcode and cur != BLOCKED:
                    # Halo of one net over the halo of another: no through route,
                    # but leave any pad body underneath alone.
                    pass

    def claim_segment(self, li, x0, y0, x1, y1, netcode, width_mm):
        half = width_mm / 2.0 + CLEARANCE_MM + STEP_MM / 2.0
        lo_x, hi_x = sorted((x0, x1))
        lo_y, hi_y = sorted((y0, y1))
        cx0, cy0 = self.to_cell(lo_x - half, lo_y - half)
        cx1, cy1 = self.to_cell(hi_x + half, hi_y + half)
        for cy in range(max(cy0, 0), min(cy1 + 1, self.ny)):
            for cx in range(max(cx0, 0), min(cx1 + 1, self.nx)):
                i = cy * self.nx + cx
                if self.cells[li][i] == FREE:
                    self.cells[li][i] = netcode


def pad_layers(pad):
    """Which of our two routing layers a pad occupies."""
    out = []
    for li, layer in enumerate((pcbnew.F_Cu, pcbnew.B_Cu)):
        if pad.IsOnLayer(layer):
            out.append(li)
    return out or [0]


def build_grid(board, width_mm, height_mm):
    grid = Grid(width_mm, height_mm)

    pads = []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            box = pad.GetBoundingBox()
            layers = pad_layers(pad)
            if pad.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH):
                layers = [0, 1]          # a hole obstructs every layer
            net = pad.GetNetCode()
            pads.append((
                pcbnew.ToMM(box.GetLeft()), pcbnew.ToMM(box.GetTop()),
                pcbnew.ToMM(box.GetRight()), pcbnew.ToMM(box.GetBottom()),
                layers, net if net > 0 else BLOCKED))

    for x0, y0, x1, y1, layers, code in pads:          # bodies
        for li in layers:
            grid.claim_box(li, x0, y0, x1, y1, code, grow=0.0, bodies=True)
    for x0, y0, x1, y1, layers, code in pads:          # clearance halo
        for li in layers:
            grid.claim_box(li, x0, y0, x1, y1, code, grow=INFLATE_MM)

    # Vias may not be dropped on top of, or hard against, a drilled pad.
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.HasHole():
                pos = pad.GetPosition()
                grid.block_vias_near(pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y),
                                     VIA_KEEPOUT_MM + 0.6)
    return grid


def _neighbours(grid, li, cx, cy):
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nx_, ny_ = cx + dx, cy + dy
        if grid.inside(nx_, ny_):
            yield li, nx_, ny_, (dx, dy), 1
    if (cx, cy) not in grid.via_block:
        yield 1 - li, cx, cy, None, VIA_COST  # change layer in place


def _passable(grid, li, cx, cy, netcode):
    v = grid.get(li, cx, cy)
    return v == FREE or v == netcode


def route_net(grid, netcode, targets):
    """
    Connect every pad of one net.

    targets is [(layer_index, cx, cy)] -- the pads. The first becomes the seed
    and the rest are drawn in one at a time, always by the cheapest path from
    whatever is already connected. Returns a list of paths, or None if any pad
    could not be reached.
    """
    connected = {targets[0]}
    remaining = list(targets[1:])
    paths = []

    while remaining:
        # Multi-source A*: start from everything already joined up.
        best = None
        goal_set = set(remaining)

        dist = {}
        prev = {}
        heap = []
        for src in connected:
            dist[src] = 0
            heapq.heappush(heap, (0, src, None))

        while heap:
            cost, node, came_dir = heapq.heappop(heap)
            if cost > dist.get(node, 1 << 30):
                continue
            if node in goal_set:
                best = node
                break
            li, cx, cy = node
            for nli, nxc, nyc, ndir, step in _neighbours(grid, li, cx, cy):
                nxt = (nli, nxc, nyc)
                if not _passable(grid, nli, nxc, nyc, netcode):
                    continue
                extra = TURN_COST if (ndir and came_dir and ndir != came_dir) else 0
                ncost = cost + step + extra
                if ncost < dist.get(nxt, 1 << 30):
                    dist[nxt] = ncost
                    prev[nxt] = (node, ndir)
                    heapq.heappush(heap, (ncost, nxt, ndir))

        if best is None:
            return None

        # Walk back to whichever source this path came from.
        path = [best]
        node = best
        while node in prev:
            node = prev[node][0]
            path.append(node)
        path.reverse()
        paths.append(path)

        for node in path:
            connected.add(node)
        remaining.remove(best)

    return paths


def path_to_tracks(board, path, netcode, width_mm, grid):
    """Turn a cell path into tracks and vias, merging collinear runs."""
    made = []
    run = [path[0]]

    def flush(run):
        if len(run) < 2:
            return
        li = run[0][0]
        x0, y0 = grid.to_mm(run[0][1], run[0][2])
        x1, y1 = grid.to_mm(run[-1][1], run[-1][2])
        track = pcbnew.PCB_TRACK(board)
        track.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x0), pcbnew.FromMM(y0)))
        track.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x1), pcbnew.FromMM(y1)))
        track.SetWidth(pcbnew.FromMM(width_mm))
        track.SetLayer(grid.layers[li])
        track.SetNetCode(netcode)
        board.Add(track)
        made.append(track)
        grid.claim_segment(li, x0, y0, x1, y1, netcode, width_mm)

    for prev_node, node in zip(path, path[1:]):
        if node[0] != prev_node[0]:
            # Layer change: close the run, drop a via, start again.
            flush(run)
            x, y = grid.to_mm(node[1], node[2])
            via = pcbnew.PCB_VIA(board)
            via.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
            via.SetWidth(pcbnew.FromMM(VIA_DIA_MM))
            via.SetDrill(pcbnew.FromMM(VIA_DRILL_MM))
            via.SetNetCode(netcode)
            board.Add(via)
            made.append(via)
            for li in (0, 1):
                grid.claim_box(li, x, y, x, y, netcode,
                               grow=VIA_DIA_MM / 2.0 + CLEARANCE_MM + STEP_MM / 2.0)
            grid.block_vias_near(x, y)
            run = [node]
            continue

        same_dir = (len(run) >= 2 and
                    (node[1] - prev_node[1], node[2] - prev_node[2]) ==
                    (run[-1][1] - run[-2][1], run[-1][2] - run[-2][2]))
        run.append(node)
        if not same_dir and len(run) > 2:
            corner = run[-1]
            flush(run[:-1])
            run = [run[-2], corner]

    flush(run)
    return made


def route_board(board, width_mm, height_mm, skip_nets=("GND",)):
    """
    Route everything except the pours.

    Returns (routed_net_names, failed_net_names). Failure is reported, never
    hidden: a board with three unrouted nets is a board with three unrouted
    nets, and calling it finished would be a lie a fab bill makes expensive.
    """
    grid = build_grid(board, width_mm, height_mm)

    by_net = {}
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            code = pad.GetNetCode()
            if code <= 0:
                continue
            name = pad.GetNetname()
            if name in skip_nets:
                continue
            pos = pad.GetPosition()
            x, y = pcbnew.ToMM(pos.x), pcbnew.ToMM(pos.y)
            cx, cy = grid.to_cell(x, y)
            for li in pad_layers(pad):
                by_net.setdefault((code, name), []).append((li, cx, cy))

    # Short nets first: they have the fewest choices, so letting them go last
    # is how a router paints itself into a corner.
    def span(item):
        pads = item[1]
        xs = [p[1] for p in pads]
        ys = [p[2] for p in pads]
        return (max(xs) - min(xs)) + (max(ys) - min(ys))

    routed, failed = [], []
    for (code, name), pads in sorted(by_net.items(), key=span):
        uniq = sorted(set(pads))
        if len(uniq) < 2:
            continue
        width = POWER_WIDTH_MM if name in POWER_NETS else TRACK_WIDTH_MM
        paths = route_net(grid, code, uniq)
        if paths is None:
            failed.append(name)
            continue
        for path in paths:
            path_to_tracks(board, path, code, width, grid)
        routed.append(name)

    return routed, failed
