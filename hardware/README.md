# BlueAudio Puck carrier board, rev A

KiCad 10 schematic and PCB for a single-board version of the puck: ESP32-WROOM-32U,
PCM5102A DAC, Li-po charging, two separate 3V3 rails, OLED header, and a 3.5 mm
output — 60 × 55 mm.

| File | What it is |
| --- | --- |
| [`blueaudio-puck.kicad_pro`](blueaudio-puck.kicad_pro) | Project — open this |
| [`blueaudio-puck.kicad_sch`](blueaudio-puck.kicad_sch) | Schematic |
| [`blueaudio-puck.kicad_pcb`](blueaudio-puck.kicad_pcb) | Board |
| [`bom.csv`](bom.csv) | Bill of materials |
| [`preview/`](preview/) | SVG renders of both |
| [`scripts/`](scripts/) | The generator — see below |

## Read this before you order anything

> [!CAUTION]
> **This board is not finished, and it is not fabricable as it stands.**
>
> It is *mostly* routed — 228 track segments, 72 vias, ground pours on both
> layers — but DRC reports **23 electrical errors** (18 clearance, 4 hole
> clearance, 1 short) and **two nets that failed to route at all: `RTS` and
> `VBUS`**.
>
> This is exactly the state this file used to warn against: a half-routed board
> looks finished. It is written down here in numbers so nobody mistakes it for
> done. Do not send these files to a fab.

> [!WARNING]
> **Nothing here has been fabricated or tested.** It passes ERC and the DRC
> checks that apply to an unrouted board. It has never been a physical object.
> Review the schematic against the datasheets before you spend money.

## Verification

Actually run, with the output quoted rather than summarised:

```
$ kicad-cli sch erc --severity-error --severity-warning
Found 2 violations                    # both lib_symbol_mismatch, see below

$ kicad-cli pcb drc --severity-error --severity-warning
Found 49 violations
Found 10 unconnected items
```

The schematic is clean: **zero electrical errors**, no unconnected pins, no
dangling labels, no malformed outline.

The board is not. Breaking the 49 down:

| Count | Kind | Status |
| --- | --- | --- |
| 18 | `clearance` | **Must fix** — copper too close |
| 4 | `hole_clearance` | **Must fix** — a drill too near copper |
| 1 | `shorting_items` | **Must fix** — two nets touching |
| 10 | `unconnected_items` | **Must fix** — `RTS` and `VBUS` never routed |
| 15 | `silk_over_copper` | Cosmetic; every fab ignores it |
| 11 | `silk_overlap` | Cosmetic |

The router in `scripts/route.py` gets 29 of 31 nets down. `RTS` and `VBUS` both
run into congestion it cannot back out of — it routes nets in order and never
rips up an earlier one to make room, which is the difference between this and a
real autorouter.

The two ERC warnings are `lib_symbol_mismatch` on `AP2112K-3.3` and
`LP2985-3.3`. Both are KiCad *derived* symbols (`extends`), and the generator
flattens them to embed them; the flattened copy differs cosmetically from the
library's. Connectivity and pin numbering are unaffected. Eeschema's
*Update Symbols from Library* clears it.

The silkscreen warnings are reference designators sitting over pads. Normal for
auto-placed boards, ignored by every fab, and worth tidying only once placement
is final.

## How it is generated

Both files come from **one** netlist, in [`scripts/design.py`](scripts/design.py).

That matters more than it sounds. A schematic and a PCB authored separately
disagree about connectivity, and KiCad does not tell you until someone runs ERC
and gets a wall of errors. One source, two emitters, no drift.

```
scripts/design.py         parts, nets, no-connects, board size  <- edit this
scripts/kisym.py          pulls symbols out of the KiCad libraries
scripts/check_sheet.py    catches stubs from different nets touching
scripts/gen_schematic.py  emits the .kicad_sch
scripts/gen_pcb.py        emits the .kicad_pcb (needs KiCad's python)
```

To regenerate:

```bash
cd hardware/scripts
python design.py          # netlist self-check
python check_sheet.py     # no two nets touching
python gen_schematic.py
"C:/Users/manup/AppData/Local/Programs/KiCad/10.0/bin/python.exe" gen_pcb.py
```

`gen_pcb.py` needs KiCad's bundled interpreter — `pcbnew` lives only there.

Symbols and footprints are copied from the installed libraries rather than
transcribed, so the output files are self-contained and open on a machine with
no libraries configured. A hand-typed pin table is exactly the kind of error
nothing but a human eye can catch.

### Once you edit it by hand

The generators overwrite their outputs. Move the files out of `hardware/`, or
stop regenerating, once you start editing in KiCad — which you will, because
routing is the next step.

## The design

```
USB-C ──> MCP73831 ──> Li-po ──┬──> AP2112K ──> +3V3  ──> ESP32, OLED, DAC digital
                               └──> LP2985  ──> +3V3A ──> DAC analogue
```

**Two regulators from the cell, not one.** The DAC's analogue rail gets its own
low-noise LP2985 with a bypass capacitor. Sharing a rail with a Bluetooth radio
is what makes a DAC hiss, and `docs/hardware.md` said so before this board
existed.

**No MCLK.** The PCM5102A has `SCK` grounded and runs from its internal PLL,
matching the hand-built prototype and what the firmware expects.

**JP1 sits in series with I²S data on GPIO 2.** That pin is a boot strapping
pin, and a DAC loading it is why the prototype needs BOOT held to flash. Lift
the jumper and the board flashes normally.

**Battery sense** is R7/R8, a 2:1 divider into GPIO 35, matching
`PUCK_BATTERY_DIVIDER_RATIO_X100=200`.

### Where it departs from `docs/hardware.md`

Both departures are deliberate and both are rev B work:

**No buck-boost.** `docs/hardware.md` specifies a TPS63020 for efficiency —
roughly 20–25% more runtime than an LDO. KiCad's standard library has no symbol
for it, and inventing one is precisely the silent error this generator exists to
avoid. Rev A uses LDOs and pays the efficiency.

**No headphone amplifier.** The PCM5102A is line level: audible on 32 Ω
headphones but quiet, exactly as the docs warn. A TPA6132A2 is the rev B
addition, and it is QFN-16 — it would make this board far harder to hand-build.

## What is left to do

1. **Fix the 23 electrical errors and route `RTS` and `VBUS`.** Either by hand
   in Pcbnew, or by giving `scripts/route.py` the ability to rip up and retry.
   Opening the board and routing two nets by hand is the shorter path.
2. **Apply JLCPCB's constraints.** Their standard two-layer process does
   0.127 mm track and space, 0.2 mm minimum drill, 0.5 mm hole-to-hole, and
   0.2 mm trace-to-outline. The board is currently drawn to KiCad's defaults
   (0.2 mm clearance, 0.3 mm drill), which is *inside* those limits and so
   safe — but the rules are not written down in the project file, so nothing
   enforces them.
3. **Add BT1 and BT3.** The firmware now has three user buttons; this board
   still has one (`SW1`, MODE). A netlist change in `design.py`.
4. **Check the antenna keep-out.** The WROOM-32U has an external connector so
   there is no PCB antenna to clear, but leave the module's edge unpoured.
5. **Re-run DRC** and get it to zero before ordering anything.
6. **Review against datasheets.** Especially the MCP73831 programming resistor
   (R5 = 2 kΩ, about 500 mA) against your cell's charge rate.
7. **Rounded corners** were dropped — arcs whose endpoints did not meet left the
   outline open, and KiCad will not guess. Add them in Pcbnew, where the editor
   keeps the ends joined.
