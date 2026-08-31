# BlueAudio Puck carrier board, rev A

KiCad 10 schematic and PCB for a single-board version of the puck: ESP32-WROOM-32U,
PCM5102A DAC, Li-po charging, two separate 3V3 rails, OLED header, three user
buttons and a 3.5 mm output — 60 × 64 mm, drawn to JLCPCB's two-layer rules.

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
> **Not finished: eight nets still need routing by hand.**
>
> Everything the generator lays is DRC-clean — **zero clearance violations,
> zero shorts, zero hole-clearance errors** under JLCPCB's rules. But 25 of 33
> nets are routed, not 33, and the remaining eight are unconnected copper:
>
> `+3V3A` · `AUDIO_L` · `CC1` · `CC2` · `DAC_LDOO` · `DAC_VNEG` · `I2S_BCK` · `I2S_LRCK`
>
> All eight are around the two fine-pitch parts — the TSSOP-20 DAC and the
> USB-C receptacle. Open the board in Pcbnew and its interactive router will
> finish them far faster and better than the one in `scripts/`. Do not order
> anything until DRC reports zero unconnected items.

> [!WARNING]
> **Nothing here has been fabricated or tested.** It passes ERC and the DRC
> checks that apply to an unrouted board. It has never been a physical object.
> Review the schematic against the datasheets before you spend money.

## Verification

Actually run, with the output quoted rather than summarised:

```
$ kicad-cli sch erc --severity-error --severity-warning
Found 4 violations                    # all lib_symbol_mismatch, see below

$ kicad-cli pcb drc --severity-error --severity-warning
Found 25 violations
Found 22 unconnected items
```

The schematic is clean: **zero electrical errors**, no unconnected pins, no
dangling labels, no malformed outline. All four ERC warnings are
`lib_symbol_mismatch` on KiCad's *derived* symbols — the two regulators and the
two transistors. Cosmetic; see below.

The board has **no electrical errors either**. Breaking the 25 down:

| Count | Kind | Status |
| --- | --- | --- |
| 22 | `unconnected_items` | **Must fix** — the eight unrouted nets |
| 15 | `silk_over_copper` | Cosmetic; every fab ignores it |
| 10 | `silk_overlap` | Cosmetic |

No `clearance`, no `shorting_items`, no `hole_clearance`. That took three fixes
to the router's geometry, each of which had produced copper that looked right
and was not — they are written up in the file that makes them:

- **A track is not a point.** A 0.4 mm power track centred on a 0.25 mm grid
  cell spills into its neighbours; checking only the cell it steps through let
  it sit 0.15 mm from a pad. Same for vias, which are 0.6 mm across — one
  landed on top of another net's track.
- **Clearance belongs in the check, not the map.** Growing a clearance halo
  around every pad *and* checking a track's own clearance against it demanded
  the gap twice, which makes escaping a 0.65 mm pitch TSSOP impossible.
- **Holes are stricter than copper.** Copper-to-hole is 0.25 mm where
  copper-to-copper is 0.15 mm, so drilled pads are claimed slightly larger than
  their copper.

The router still gets 25 of 33 nets rather than all of them. It routes in order
and never rips up an earlier net to make room, which is the difference between
it and a real autorouter.

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

**Three user buttons** on GPIO 32/33/27, matching the firmware — SW1 is BT1
(next / volume up), SW2 is BT2 (play-pause / pairing), SW3 is BT3 (previous /
volume down). All active low straight to ground; the ESP32's internal pull-ups
mean no external resistors. SW4 and SW5 are RESET and BOOT.

Note there is **no LED on GPIO 27** on this board. The Kconfig help for
`PUCK_LED_GPIO` suggests 27 for an external status LED, which would collide
with BT3 on a breadboard. Here the only LED is D1, the charge indicator on
`CHG_STAT`, so GPIO 27 is unambiguously BT3.

### Where it departs from `docs/hardware.md`

Both departures are deliberate and both are rev B work:

**No buck-boost.** `docs/hardware.md` specifies a TPS63020 for efficiency —
roughly 20–25% more runtime than an LDO. KiCad's standard library has no symbol
for it, and inventing one is precisely the silent error this generator exists to
avoid. Rev A uses LDOs and pays the efficiency.

**No headphone amplifier.** The PCM5102A is line level: audible on 32 Ω
headphones but quiet, exactly as the docs warn. A TPA6132A2 is the rev B
addition, and it is QFN-16 — it would make this board far harder to hand-build.

## Manufacturing rules

Written into the project file, so DRC enforces them rather than leaving them as
folklore. JLCPCB's standard two-layer process, with margin:

| | JLC minimum | Used here |
| --- | --- | --- |
| Track width | 0.127 mm | 0.20 mm |
| Clearance | 0.127 mm | 0.15 mm |
| Via drill | 0.20 mm | 0.30 mm |
| Via diameter | drill + 0.26 | 0.60 mm |
| Annular ring | 0.13 mm | 0.15 mm |
| Hole to hole | 0.50 mm | 0.50 mm |
| Track to outline | 0.20 mm | 0.50 mm |

Only clearance sits near a limit, and it is still 18% above JLC's floor. The
0.05 mm it frees up per side compared with KiCad's 0.2 mm default is most of
what lets the congested area route at all.

**Teardrops are configured but not generated.** The parameters are in the
project file; KiCad adds the actual teardrops on demand. Run *Edit → Teardrops
→ Add Teardrops* in Pcbnew once routing is finished — doing it before then
would only have to be redone.

## What is left to do

1. **Route the last eight nets** in Pcbnew: `+3V3A`, `AUDIO_L`, `CC1`, `CC2`,
   `DAC_LDOO`, `DAC_VNEG`, `I2S_BCK`, `I2S_LRCK`. All are around the TSSOP-20
   DAC and the USB-C part. The interactive router will do this in minutes.
2. **Add teardrops** once routing is done.
3. **Check the antenna keep-out.** The WROOM-32U has an external connector so
   there is no PCB antenna to clear, but leave the module's edge unpoured.
4. **Re-run DRC** and get it to zero before ordering anything.
5. **Review against datasheets.** Especially the MCP73831 programming resistor
   (R5 = 2 kΩ, about 500 mA) against your cell's charge rate.
6. **Rounded corners** were dropped — arcs whose endpoints did not meet left the
   outline open, and KiCad will not guess. Add them in Pcbnew, where the editor
   keeps the ends joined.
