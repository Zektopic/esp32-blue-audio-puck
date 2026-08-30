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

> [!WARNING]
> **The board is not routed.** There are no tracks and no copper pours. Every
> footprint is placed and every pad carries its correct net, so opening it in
> Pcbnew shows a complete ratsnest — but nothing is connected in copper yet.
>
> That is deliberate. A half-routed board looks finished and is worse than an
> obviously unrouted one, and the analogue section here needs judgement — star
> ground, keeping the DAC's return path away from the radio, the charge pump's
> flying capacitor loop — that does not come out of arithmetic.

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
Found 59 violations                   # 38 silk-over-copper, 21 silk overlap
Found 111 unconnected items           # expected: the board is not routed
```

**Zero electrical errors.** No unconnected pins, no dangling labels, no
clearance violations, no shorts, no malformed outline.

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

1. **Route it.** Two layers is enough. Ground pour on the back, keep the DAC's
   analogue return separate from the radio's, keep C8 (the charge pump's flying
   capacitor) loop tight.
2. **Check the antenna keep-out.** The WROOM-32U has an external connector so
   there is no PCB antenna to clear, but leave the module's edge unpoured.
3. **Re-run DRC** once routed, with track width and clearance set for your fab.
4. **Review against datasheets.** Especially the MCP73831 programming resistor
   (R5 = 2 kΩ, about 500 mA) against your cell's charge rate.
5. **Rounded corners** were dropped — arcs whose endpoints did not meet left the
   outline open, and KiCad will not guess. Add them in Pcbnew, where the editor
   keeps the ends joined.
