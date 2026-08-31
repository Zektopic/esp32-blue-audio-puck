"""
BlueAudio Puck carrier board, revision A -- the single source of truth.

The schematic and the PCB are both generated from this file. That is the whole
point: two files authored independently will disagree about connectivity, and
KiCad will not say so until someone runs ERC and gets a wall of errors. One
netlist, two emitters, no drift.

Design intent, and where it departs from docs/hardware.md:

  * ESP32-WROOM-32U, because a puck lives in a pocket with a body between it
    and the phone, and an external antenna measurably beats the PCB one.
  * PCM5102A with SCK grounded, so it runs from its internal PLL and needs no
    master clock wire -- the same arrangement the current hand-built prototype
    uses, and what the firmware expects.
  * Two separate regulators from the cell. The digital rail is an AP2112K; the
    DAC's analogue rail is an LP2985, a genuinely low-noise part. Sharing one
    rail is what makes a DAC hiss, and hardware.md already says so.
  * DEPARTURE: hardware.md specifies a TPS63020 buck-boost for efficiency.
    KiCad's standard library has no symbol for it, and inventing one is the
    kind of silent error this file exists to avoid. Revision A uses LDOs and
    accepts the efficiency loss; the buck-boost is a revision B change.
  * DEPARTURE: no headphone amplifier. The PCM5102A is line level -- audible on
    32 ohm headphones but quiet, exactly as hardware.md warns. A TPA6132A2 is
    the revision B addition; it is QFN-16 and would make this board much harder
    to assemble by hand.
  * Auto-reset. Q1 and Q2 are the cross-coupled pair every ESP devkit uses, so
    esptool can reset the chip and pull it into the bootloader without anyone
    holding a button. The behaviour that matters is that opening a serial port
    which asserts *both* DTR and RTS must NOT reset the chip:

        DTR RTS  Q1(EN)  Q2(IO0)  result
         1   1    off     off     runs normally
         0   0    off     off     runs normally
         1   0    off     ON      IO0 low  -> bootloader
         0   1    ON      off     EN  low  -> reset

    No base resistors, matching the NodeMCU/devkit reference this copies. SW3
    is still fitted as the manual fallback.
  * JP1 sits in series with I2S data on GPIO 2. That pin is a boot strapping
    pin, and a DAC loading it is why the prototype needs BOOT held to flash.
    Lift the jumper and the board flashes normally.
"""

# ---------------------------------------------------------------------------
# Parts. (library, symbol, footprint, value, description)
# ---------------------------------------------------------------------------

PARTS = {
    "U1": ("RF_Module", "ESP32-WROOM-32U", "RF_Module:ESP32-WROOM-32U",
           "ESP32-WROOM-32U", "Bluetooth Classic SoC module, external antenna"),
    "U2": ("Audio", "PCM5102A", "Package_SO:TSSOP-20_4.4x6.5mm_P0.65mm",
           "PCM5102A", "Stereo I2S DAC, internal PLL, line level out"),
    "U3": ("Battery_Management", "MCP73831-2-OT", "Package_TO_SOT_SMD:SOT-23-5",
           "MCP73831-2-OT", "Single-cell Li-po charger"),
    "U4": ("Regulator_Linear", "AP2112K-3.3", "Package_TO_SOT_SMD:SOT-23-5",
           "AP2112K-3.3", "3V3 digital rail"),
    "U5": ("Regulator_Linear", "LP2985-3.3", "Package_TO_SOT_SMD:SOT-23-5",
           "LP2985-3.3", "3V3 low-noise analogue rail for the DAC"),

    "J1": ("Connector", "USB_C_Receptacle_USB2.0_16P",
           "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
           "USB-C", "Charging only; no USB data"),
    "J2": ("Connector_Audio", "AudioJack3",
           "Connector_Audio:Jack_3.5mm_CUI_SJ1-3513N_Horizontal",
           "3.5mm", "Stereo headphone output"),
    "J3": ("Connector", "Conn_01x04_Pin",
           "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
           "OLED", "SSD1306 I2C module: GND VCC SCL SDA"),
    "J4": ("Connector", "Conn_01x02_Pin",
           "Connector_JST:JST_PH_S2B-PH-K_1x02_P2.00mm_Horizontal",
           "BATT", "Li-po cell"),
    "J5": ("Connector", "Conn_01x06_Pin",
           "Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical",
           "PROG", "GND 3V3 RXD TXD DTR RTS -- plugs onto a CP2102/FTDI adapter"),

    "SW1": ("Switch", "SW_Push", "Button_Switch_SMD:Panasonic_EVQPUJ_EVQPUA",
            "MODE", "Play/pause, track skip, pairing"),
    "SW2": ("Switch", "SW_Push", "Button_Switch_SMD:Panasonic_EVQPUJ_EVQPUA",
            "RESET", "Pulls EN low"),
    "SW3": ("Switch", "SW_Push", "Button_Switch_SMD:Panasonic_EVQPUJ_EVQPUA",
            "BOOT", "Pulls IO0 low; the manual fallback if auto-reset misbehaves"),

    # Cross-coupled auto-reset pair. See AUTO_RESET_TRUTH_TABLE below.
    "Q1": ("Transistor_BJT", "MMBT3904", "Package_TO_SOT_SMD:SOT-23",
           "MMBT3904", "Auto-reset: drives EN from DTR/RTS"),
    "Q2": ("Transistor_BJT", "MMBT3904", "Package_TO_SOT_SMD:SOT-23",
           "MMBT3904", "Auto-reset: drives IO0 from DTR/RTS"),

    "D1": ("Device", "LED", "LED_SMD:LED_0805_2012Metric", "CHG",
           "Charge status from the MCP73831"),

    "JP1": ("Device", "R", "Resistor_SMD:R_0805_2012Metric", "0R",
            "Series link on I2S data; lift to flash, GPIO 2 is a strapping pin"),

    "R1": ("Device", "R", "Resistor_SMD:R_0805_2012Metric", "10k", "EN pull-up"),
    "R2": ("Device", "R", "Resistor_SMD:R_0805_2012Metric", "10k", "IO0 pull-up"),
    "R3": ("Device", "R", "Resistor_SMD:R_0805_2012Metric", "5k1", "USB-C CC1 pulldown"),
    "R4": ("Device", "R", "Resistor_SMD:R_0805_2012Metric", "5k1", "USB-C CC2 pulldown"),
    "R5": ("Device", "R", "Resistor_SMD:R_0805_2012Metric", "2k",
           "Charge current program, ~500 mA"),
    "R6": ("Device", "R", "Resistor_SMD:R_0805_2012Metric", "1k", "Charge LED"),
    "R7": ("Device", "R", "Resistor_SMD:R_0805_2012Metric", "100k",
           "Battery divider, high side"),
    "R8": ("Device", "R", "Resistor_SMD:R_0805_2012Metric", "100k",
           "Battery divider, low side"),
    "R9": ("Device", "R", "Resistor_SMD:R_0805_2012Metric", "10k",
           "XSMT pull-up: releases the DAC soft-mute"),

    "C1":  ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "100n", "U1 VDD"),
    "C2":  ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "10u", "U1 bulk"),
    "C3":  ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "1u", "EN delay"),
    "C4":  ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "100n", "U2 DVDD"),
    "C5":  ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "100n", "U2 AVDD"),
    "C6":  ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "10u", "U2 AVDD bulk"),
    "C7":  ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "100n", "U2 CPVDD"),
    "C8":  ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "1u", "Charge pump flying cap"),
    "C9":  ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "1u", "VNEG reservoir"),
    "C10": ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "100n", "U2 internal LDO"),
    "C11": ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "1u", "U3 input"),
    "C12": ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "4u7", "U3 output"),
    "C13": ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "1u", "U4 input"),
    "C14": ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "10u", "U4 output"),
    "C15": ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "1u", "U5 input"),
    "C16": ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "10u", "U5 output"),
    "C17": ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "10n",
            "U5 bypass: this cap is what makes the LP2985 low noise"),
    "C18": ("Device", "C", "Capacitor_SMD:C_0805_2012Metric", "100n",
            "Battery sense filter"),
}

# ---------------------------------------------------------------------------
# Nets. Power nets are named so KiCad treats them as such.
# ---------------------------------------------------------------------------

NETS = {
    "GND": [
        ("U1", "1"), ("U1", "15"), ("U1", "38"), ("U1", "39"),
        ("U2", "3"), ("U2", "9"), ("U2", "10"), ("U2", "11"), ("U2", "12"),
        ("U2", "16"), ("U2", "19"),
        ("U3", "2"), ("U4", "2"), ("U5", "2"),
        ("J1", "A1"), ("J1", "A12"), ("J1", "B1"), ("J1", "B12"), ("J1", "SH"),
        ("J2", "S"), ("J3", "1"), ("J4", "2"), ("J5", "1"),
        ("SW1", "2"), ("SW2", "2"), ("SW3", "2"),
        ("R3", "2"), ("R4", "2"), ("R8", "2"),
        ("C1", "2"), ("C2", "2"), ("C3", "2"), ("C4", "2"), ("C5", "2"),
        ("C6", "2"), ("C7", "2"), ("C9", "2"), ("C10", "2"), ("C11", "2"),
        ("C12", "2"), ("C13", "2"), ("C14", "2"), ("C15", "2"), ("C16", "2"),
        ("C17", "2"), ("C18", "2"),
        ("R5", "2"),
    ],
    "VBUS": [
        ("J1", "A4"), ("J1", "A9"), ("J1", "B4"), ("J1", "B9"),
        ("U3", "4"), ("C11", "1"), ("R6", "1"),
    ],
    "VBAT": [
        ("U3", "3"), ("J4", "1"), ("C12", "1"),
        ("U4", "1"), ("C13", "1"),
        ("U5", "1"), ("C15", "1"),
        ("R7", "1"),
    ],
    "+3V3": [                      # digital rail
        ("U4", "5"), ("U4", "3"), ("C14", "1"),
        ("U1", "2"), ("C1", "1"), ("C2", "1"),
        ("U2", "20"), ("C4", "1"),
        ("R1", "1"), ("R2", "1"), ("R9", "1"),
        ("J3", "2"), ("J5", "2"),
    ],
    "+3V3A": [                     # analogue rail, DAC only
        ("U5", "5"), ("U5", "3"), ("C16", "1"),
        ("U2", "8"), ("C5", "1"), ("C6", "1"),
        ("U2", "1"), ("C7", "1"),
    ],
    "U5_BYPASS": [("U5", "4"), ("C17", "1")],

    # EN and IO0 are no longer brought out to the header: the transistors
    # derive them, and exposing both would let an adapter fight them.
    "EN": [("U1", "3"), ("R1", "2"), ("C3", "1"), ("SW2", "1"), ("Q1", "3")],
    "IO0": [("U1", "25"), ("R2", "2"), ("SW3", "1"), ("Q2", "3")],
    "RXD0": [("U1", "34"), ("J5", "3")],
    "TXD0": [("U1", "35"), ("J5", "4")],

    # Q1 base = RTS, emitter = DTR; Q2 base = DTR, emitter = RTS. The
    # cross-coupling is the whole trick -- see AUTO_RESET_TRUTH_TABLE.
    "DTR": [("J5", "5"), ("Q1", "2"), ("Q2", "1")],
    "RTS": [("J5", "6"), ("Q2", "2"), ("Q1", "1")],

    "I2S_BCK": [("U1", "26"), ("U2", "13")],
    "I2S_LRCK": [("U1", "23"), ("U2", "15")],
    "I2S_DATA_MCU": [("U1", "24"), ("JP1", "1")],
    "I2S_DATA": [("JP1", "2"), ("U2", "14")],

    "I2C_SDA": [("U1", "33"), ("J3", "4")],
    "I2C_SCL": [("U1", "36"), ("J3", "3")],

    "BTN_MODE": [("U1", "9"), ("SW1", "1")],
    "VBAT_SENSE": [("U1", "7"), ("R7", "2"), ("R8", "1"), ("C18", "1")],

    "DAC_XSMT": [("U2", "17"), ("R9", "2")],
    "DAC_LDOO": [("U2", "18"), ("C10", "1")],
    "DAC_CAPP": [("U2", "2"), ("C8", "1")],
    "DAC_CAPM": [("U2", "4"), ("C8", "2")],
    "DAC_VNEG": [("U2", "5"), ("C9", "1")],
    "AUDIO_L": [("U2", "6"), ("J2", "T")],
    "AUDIO_R": [("U2", "7"), ("J2", "R")],

    "CC1": [("J1", "A5"), ("R3", "1")],
    "CC2": [("J1", "B5"), ("R4", "1")],
    "CHG_PROG": [("U3", "5"), ("R5", "1")],
    "CHG_STAT": [("U3", "1"), ("D1", "1")],
    "CHG_LED_A": [("D1", "2"), ("R6", "2")],
}

# Pins with nothing attached, declared so ERC reports a clean sheet rather than
# a list of warnings the reader has to learn to ignore.
NO_CONNECT = [
    ("U4", "4"),                                  # AP2112K NC
    # USB data and sideband: this port charges only.
    ("J1", "A6"), ("J1", "A7"), ("J1", "B6"), ("J1", "B7"),
    ("J1", "A8"), ("J1", "B8"),
    # ESP32 pins this design does not use.
    ("U1", "4"), ("U1", "5"), ("U1", "6"), ("U1", "8"),
    ("U1", "10"), ("U1", "11"), ("U1", "12"), ("U1", "13"), ("U1", "14"),
    ("U1", "16"), ("U1", "17"), ("U1", "18"), ("U1", "19"), ("U1", "20"),
    ("U1", "21"), ("U1", "22"), ("U1", "27"), ("U1", "28"), ("U1", "29"),
    ("U1", "30"), ("U1", "31"), ("U1", "32"), ("U1", "37"),
]

# Footprint pads with no pad number: mounting tabs and shells. They have no
# schematic pin, so the netlist cannot reach them, but leaving a metal tab
# floating is worse than tying it down. Grounding them is the usual choice and
# is what makes the jack mechanically and electrically quiet.
MECHANICAL_TO_GND = ["J1"]

# Bypass capacitors and the pin each one serves. A decoupling cap 20 mm from
# its pin is not decoupling anything -- the loop inductance defeats the point
# entirely -- so the placer puts each of these hard against its target pad
# rather than filing it under "power components".
DECOUPLING = {
    "C1": ("U1", "2"),      # ESP32 VDD
    "C2": ("U1", "2"),      # ESP32 bulk
    "C4": ("U2", "20"),     # DAC DVDD
    "C5": ("U2", "8"),      # DAC AVDD
    "C6": ("U2", "8"),      # DAC AVDD bulk
    "C7": ("U2", "1"),      # DAC CPVDD
    "C8": ("U2", "2"),      # charge pump flying cap -- the tightest loop here
    "C9": ("U2", "5"),      # VNEG reservoir
    "C10": ("U2", "18"),    # DAC internal LDO
    "C11": ("U3", "4"),     # charger input
    "C12": ("U3", "3"),     # charger output
    "C13": ("U4", "1"),     # digital LDO input
    "C14": ("U4", "5"),     # digital LDO output
    "C15": ("U5", "1"),     # analogue LDO input
    "C16": ("U5", "5"),     # analogue LDO output
    "C17": ("U5", "4"),     # analogue LDO bypass -- what makes it low noise
}

BOARD = {
    "name": "BlueAudio Puck carrier, rev A",
    # 60 x 55 leaves room to place every part on the front. Moving the
    # passives to the back would shrink it considerably, but a single-sided
    # assembly is far easier to hand-build, and this is revision A.
    "width_mm": 60.0,
    "height_mm": 55.0,
    "corner_radius_mm": 6.0,
    "mounting_hole_dia_mm": 2.2,
    "mounting_inset_mm": 4.0,
}


def pin_to_net():
    """(ref, pin) -> net name, for the emitters."""
    out = {}
    for net, pins in NETS.items():
        for ref, pin in pins:
            if (ref, pin) in out:
                raise ValueError("%s.%s is on both %s and %s"
                                 % (ref, pin, out[(ref, pin)], net))
            out[(ref, pin)] = net
    return out


def auto_reset_truth_table():
    """
    Derive the auto-reset behaviour from the netlist and check it.

    Reasoning about a cross-coupled pair from memory is how people ship boards
    that reset every time a terminal opens. This reads the actual connectivity
    -- which net is on each transistor's base, emitter and collector -- and
    works out what happens for all four DTR/RTS combinations. MMBT3904 pins are
    1=B, 2=E, 3=C.
    """
    mapping = pin_to_net()
    rows = []

    for dtr in (0, 1):
        for rts in (0, 1):
            levels = {"DTR": dtr, "RTS": rts}
            state = {}
            for q, driven in (("Q1", "EN"), ("Q2", "IO0")):
                base = mapping[(q, "1")]
                emitter = mapping[(q, "2")]
                collector = mapping[(q, "3")]
                if collector != driven:
                    raise ValueError("%s collector is on %s, expected %s"
                                     % (q, collector, driven))
                # An NPN conducts when its base sits above its emitter.
                on = levels.get(base, 0) > levels.get(emitter, 0)
                # A conducting transistor pulls its collector to its emitter;
                # otherwise the pull-up holds the pin high.
                state[driven] = levels.get(emitter, 0) if on else 1
            rows.append((dtr, rts, state["EN"], state["IO0"]))
    return rows


AUTO_RESET_EXPECTED = [
    # dtr rts  EN IO0
    (0, 0, 1, 1),   # both deasserted: runs
    (0, 1, 0, 1),   # EN low: reset
    (1, 0, 1, 0),   # IO0 low: bootloader
    (1, 1, 1, 1),   # both asserted: runs -- opening a port must not reset
]


def check():
    """Catch the mistakes that are cheap to make in a table this size."""
    problems = []
    mapping = pin_to_net()

    for ref, _pin in list(mapping) + NO_CONNECT:
        if ref not in PARTS:
            problems.append("net references unknown part %s" % ref)

    for net, pins in NETS.items():
        if len(pins) < 2:
            problems.append("net %s has only %d connection" % (net, len(pins)))

    for ref, pin in NO_CONNECT:
        if (ref, pin) in mapping:
            problems.append("%s.%s is both no-connect and on net %s"
                            % (ref, pin, mapping[(ref, pin)]))

    if sorted(auto_reset_truth_table()) != sorted(AUTO_RESET_EXPECTED):
        problems.append("auto-reset truth table is wrong: %s"
                        % (auto_reset_truth_table(),))

    for cap, (ic, pin) in DECOUPLING.items():
        if cap not in PARTS or ic not in PARTS:
            problems.append("decoupling entry %s -> %s.%s names an unknown part"
                            % (cap, ic, pin))
        elif (ic, pin) not in mapping:
            problems.append("decoupling target %s.%s is on no net" % (ic, pin))

    return sorted(set(problems))


if __name__ == "__main__":
    issues = check()
    print("%d parts, %d nets, %d no-connects"
          % (len(PARTS), len(NETS), len(NO_CONNECT)))
    if issues:
        print("PROBLEMS:")
        for p in issues:
            print("  -", p)
        raise SystemExit(1)
    print("netlist self-check passed")
    print("auto-reset truth table (DTR RTS -> EN IO0):")
    for dtr, rts, en, io0 in sorted(auto_reset_truth_table()):
        what = ("runs" if (en and io0) else
                "RESET" if not en else "BOOTLOADER")
        print("   %d   %d  ->  %d  %d   %s" % (dtr, rts, en, io0, what))
