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
           "PROG", "3V3 GND EN IO0 TXD RXD"),

    "SW1": ("Switch", "SW_Push", "Button_Switch_SMD:Panasonic_EVQPUJ_EVQPUA",
            "MODE", "Play/pause, track skip, pairing"),
    "SW2": ("Switch", "SW_Push", "Button_Switch_SMD:Panasonic_EVQPUJ_EVQPUA",
            "RESET", "Pulls EN low"),

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
        ("J2", "S"), ("J3", "1"), ("J4", "2"), ("J5", "2"),
        ("SW1", "2"), ("SW2", "2"),
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
        ("J3", "2"), ("J5", "1"),
    ],
    "+3V3A": [                     # analogue rail, DAC only
        ("U5", "5"), ("U5", "3"), ("C16", "1"),
        ("U2", "8"), ("C5", "1"), ("C6", "1"),
        ("U2", "1"), ("C7", "1"),
    ],
    "U5_BYPASS": [("U5", "4"), ("C17", "1")],

    "EN": [("U1", "3"), ("R1", "2"), ("C3", "1"), ("SW2", "1"), ("J5", "3")],
    "IO0": [("U1", "25"), ("R2", "2"), ("J5", "4")],
    "TXD0": [("U1", "35"), ("J5", "5")],
    "RXD0": [("U1", "34"), ("J5", "6")],

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
