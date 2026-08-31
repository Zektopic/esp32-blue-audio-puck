"""
Pull symbol definitions out of the installed KiCad libraries.

A .kicad_sch embeds every symbol it uses, so the generated schematic is
self-contained and opens on a machine with no libraries configured. Symbols are
copied from the installed libraries rather than transcribed by hand -- a
hand-typed pin table is the kind of mistake nothing but a human eye can catch.

The one wrinkle is `extends`: KiCad derives many parts (AP2112K-3.3 from
AP2204K-1.5, say) instead of repeating the drawing. Those have to be flattened
before they can be embedded, which is what resolve() does.
"""

import io
import os
import re

KICAD_SHARE = r"C:\Users\manup\AppData\Local\Programs\KiCad\10.0\share\kicad"
SYMBOL_DIR = os.path.join(KICAD_SHARE, "symbols")

_lib_cache = {}


def _load(lib):
    if lib not in _lib_cache:
        path = os.path.join(SYMBOL_DIR, lib + ".kicad_sym")
        _lib_cache[lib] = io.open(path, encoding="utf-8").read()
    return _lib_cache[lib]


def _block_at(text, start):
    """Return the balanced s-expression beginning at `start`."""
    depth = 0
    in_str = False
    i = start
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1
    raise ValueError("unbalanced s-expression")


def raw_symbol(lib, name):
    text = _load(lib)
    needle = '(symbol "%s"' % name
    i = text.find(needle)
    if i < 0:
        raise KeyError("%s not found in %s" % (name, lib))
    return _block_at(text, i)


def _children(block):
    """Top-level sub-expressions of a symbol block, in order."""
    # Skip past `(symbol "NAME"` before scanning.
    head = block.index('"', block.index("(symbol"))
    head = block.index('"', head + 1) + 1
    out = []
    i = head
    while i < len(block):
        if block[i] == "(":
            sub = _block_at(block, i)
            out.append(sub)
            i += len(sub)
        else:
            i += 1
    return out


def resolve(lib, name):
    """
    Return a flattened symbol block ready to embed in lib_symbols.

    A derived symbol keeps its own properties and inherits the parent's units,
    which carry the graphics and the pins. The unit names are prefixed with the
    parent's name, so they have to be renamed or KiCad will not associate them.
    """
    block = raw_symbol(lib, name)
    m = re.search(r'\(extends "([^"]+)"\)', block)
    if not m:
        return block

    parent_name = m.group(1)
    parent = raw_symbol(lib, parent_name)

    own = [c for c in _children(block) if not c.startswith("(extends")]
    inherited = [c for c in _children(parent) if c.startswith('(symbol "')]

    # Units are named "<parent>_0_1", "<parent>_1_1" and must follow the child.
    renamed = []
    for unit in inherited:
        renamed.append(unit.replace('(symbol "%s_' % parent_name,
                                    '(symbol "%s_' % name, 1))

    body = "\n".join(own + renamed)
    return '(symbol "%s"\n%s\n)' % (name, body)


def pins(lib, name):
    """
    Pin geometry for a symbol, in millimetres relative to its origin.

    The `at` coordinate IS the connection point -- the free end of the pin --
    and `angle` points from there *into* the symbol body, which the pin spans
    for `length`. Getting that backwards puts every wire one pin-length inside
    the part, where it touches nothing: the sheet looks wired and ERC reports
    hundreds of dangling labels.

    So `connect` is the anchor, and `outward` is the direction a stub should
    leave in, in symbol space.
    """
    block = resolve(lib, name)
    found = []
    for m in re.finditer(
            r'\(pin\s+(\S+)\s+(\S+)\s*\(at\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s*\)\s*'
            r'\(length\s+([\d.]+)\s*\)',
            block):
        etype, _shape, x, y, ang, length = m.groups()
        tail = block[m.end():m.end() + 400]
        num = re.search(r'\(number "([^"]+)"', tail)
        nam = re.search(r'\(name "([^"]+)"', tail)
        x, y, ang, length = float(x), float(y), float(ang), float(length)

        # Angle points from the connection end towards the body.
        dx = {0: 1.0, 90: 0.0, 180: -1.0, 270: 0.0}[int(ang) % 360]
        dy = {0: 0.0, 90: 1.0, 180: 0.0, 270: -1.0}[int(ang) % 360]
        found.append({
            "number": num.group(1) if num else "?",
            "name": nam.group(1) if nam else "?",
            "type": etype,
            "connect": (x, y),
            "outward": (-dx, -dy),
            "body_end": (x + dx * length, y + dy * length),
            "angle": int(ang) % 360,
        })
    return found


if __name__ == "__main__":
    import sys
    lib, name = sys.argv[1], sys.argv[2]
    blk = resolve(lib, name)
    print("resolved block: %d chars" % len(blk))
    for p in pins(lib, name):
        print("  pin %-4s %-14s %-12s connect=%s out=%s"
              % (p["number"], p["name"], p["type"], p["connect"], p["outward"]))
