# Hardware

Parts, wiring, power budget and the route from a breadboard to a puck.

> [!WARNING]
> **Nothing on this page has been verified against built hardware.** The
> prototype is an ESP32 devkit with no DAC attached. Pin assignments are
> proposals; power figures are datasheet-derived estimates, not measurements.

## Why the original ESP32

A2DP rides on Bluetooth Classic (BR/EDR). Espressif removed Classic from every
chip after the original ESP32, so:

| Part | Bluetooth | A2DP sink? |
| --- | --- | --- |
| ESP32 (Xtensa) | Classic BR/EDR + BLE | **Yes** |
| ESP32-S3 | BLE 5.0 only | No |
| ESP32-C3 | BLE 5.0 only | No |

The S3 and C3 cannot fall back to LE Audio either — that needs BLE 5.2
isochronous channels and they are 5.0. This is silicon, not firmware.

Use a **WROOM-32U with an external antenna** if you can. A puck lives in a
pocket or bag with a body between it and the phone, and builders consistently
report better range with an external antenna than with the PCB one.

## Phase 0 — breadboard

What the current firmware targets.

| ESP32 | PCM5102A | Signal |
| --- | --- | --- |
| GPIO 26 | BCK | Bit clock |
| GPIO 25 | LRCK | Word select (L/R) |
| GPIO 22 | DIN | Serial data |
| 3V3 | VIN | Power |
| GND | GND | Ground |

Plus a button from GPIO 33 to ground, and a LED on GPIO 2 (already fitted on
most devkits).

### PCM5102A jumpers

These cause most of the "no sound" reports on these breakouts.

| Pin | Tie to | Why |
| --- | --- | --- |
| SCK | GND | Selects the internal PLL. Floating gives silence. |
| XSMT | 3V3 | Releases soft-mute. Low means muted. |
| FMT | GND | I²S (Philips) format — matches the firmware default |
| FLT | GND | Normal latency filter |
| DEMP | GND | De-emphasis off |

If your breakout ties `FMT` high instead, switch the firmware to left-justified
under **BlueAudio Puck → audio output → I²S frame format**. A format mismatch
produces audio that is present but wrong, which is far harder to diagnose than
silence.

### Expectation setting

The PCM5102A is a **line-level output, not a headphone driver**. It will be
audible on 32 Ω headphones but quiet. That is fine for proving the chain; a real
headphone amplifier is the next phase, not this one.

## Phase 1 — proper analogue path

| Function | Part | Notes |
| --- | --- | --- |
| DAC + headphone amp | **CS43131** | One package, low power, drives 32 Ω comfortably. What commercial dongles use. |
| Alternative DAC | PCM5102A | Line level only |
| Alternative amp | TPA6132A2 | Pairs with the PCM5102A; more parts, easier to hand-solder |

## Phase 2 — power

| Function | Part | Notes |
| --- | --- | --- |
| Cell | 500–1000 mAh Li-po | Size decides runtime and therefore enclosure dimensions |
| Charger | BQ25180 | I²C status; MCP73831 if you want simple |
| Main rail | TPS63020 buck-boost | ~88–90% across the cell's range, and keeps working down to 3.0 V |
| DAC analogue rail | TPS7A20 LDO | Low noise, fed from the switcher's output |

**Do not run the DAC's analogue supply from the switcher directly.** Audio DACs
are unforgiving of noisy rails and you will hear it as hiss. Two-stage —
buck-boost to ~3.5 V for the ESP32, then a low-noise LDO down to 3.3 V for the
DAC's analogue pin only — costs almost nothing at a 25 mA load.

An LDO straight from the cell would be simpler, but its efficiency is
V_out ÷ V_in: 79% at a full 4.2 V cell, and it drops out around 3.4 V, stranding
the bottom of the cell's capacity. The buck-boost is worth roughly 20–25% more
runtime for one part.

## Power budget

> **Estimates only.** Datasheet figures and typical reported values. Nothing
> here has been measured on this build, and measuring on a devkit would not
> help — the CP210x bridge and the AMS1117 regulator dominate a devkit's draw
> whatever the firmware does.

At the 3.3 V rail while streaming:

| Component | Draw |
| --- | --- |
| ESP32 @ 160 MHz, BT Classic active | ~110 mA |
| PCM5102A | ~25 mA |
| Headphone amp (TPA6132A2 class) | ~10 mA |
| Status LED (averaged) | ~1 mA |
| **Total** | **~146 mA (≈482 mW)** |

The ESP32 is roughly three quarters of the budget. Every optimisation that is
not about the ESP32 is rearranging deck chairs.

By state:

| State | Draw | What is running |
| --- | --- | --- |
| Playing | ~146 mA | Everything |
| Connected, paused | ~65 mA | Link in a low-power mode, DAC muted |
| Deep sleep | ~10 µA | Custom board only |

Estimated runtime through an ~88% converter (≈148 mA from a 3.7 V nominal cell):

| Cell | Runtime |
| --- | --- |
| 500 mAh | ~3 h |
| 1000 mAh | ~6 h |
| 2000 mAh | ~12 h |

### Levers, ranked by what they actually buy

1. **A bigger cell.** Unglamorous, and by far the biggest lever. A 1000 mAh
   pouch is a couple of millimetres thicker and doubles runtime. Do this before
   any firmware cleverness.
2. **Buck-boost instead of an LDO.** ~20–25%.
3. **160 MHz instead of 240.** ~15%. Already the default here.
4. **Lower Bluetooth TX power.** Already capped at 0 dBm
   (`PUCK_BT_TX_POWER_LEVEL`); worth several mA.
5. **Deep sleep on disconnect.** Extends playback by nothing, but it is the
   difference between flat by morning and fine next week. Already implemented.
6. **Power-gate the headphone amp** using the 3.5 mm jack's switched contact and
   a load switch. Free hardware you already have in the connector.

### The honest comparison

A Qudelix 5K runs ~20 hours; a FiiO BTR7 ~9. They use purpose-built Bluetooth
audio SoCs that idle in single-digit milliamps with hardware codec engines. This
is a general-purpose MCU running a full software Bluetooth stack, and no amount
of tuning closes that gap.

That is the trade for firmware you control. Size the cell accordingly rather
than trying to optimise your way to parity.

## Codecs

Stock ESP-IDF is **SBC only**, which is the usual argument for eventually
replacing the ESP32 with a dedicated part like a Microchip BM83.

That argument is weaker than it looks. The
[ESP32-A2DP-SINK-WITH-CODECS-UPDATED](https://github.com/WillyBilly06/ESP32-A2DP-SINK-WITH-CODECS-UPDATED)
project patches LDAC, aptX, aptX-HD and AAC into the IDF Bluetooth stack. The
codec ceiling is a firmware limit, not a silicon one — worth establishing before
planning a board respin around a different SoC.

## Roadmap

```mermaid
flowchart LR
    P0["Phase 0<br/>devkit + PCM5102A<br/>USB power"] --> P1["Phase 1<br/>CS43131<br/>headphone amp"]
    P1 --> P2["Phase 2<br/>Li-po, buck-boost<br/>low-noise LDO"]
    P2 --> P3["Phase 3<br/>custom PCB<br/>printed enclosure"]
```

Nothing in the firmware is wasted if the brain changes later: the DAC, power,
enclosure and UI work all carry over, and only `bt_core` and `puck_avrcp` are
Bluetooth-specific.
