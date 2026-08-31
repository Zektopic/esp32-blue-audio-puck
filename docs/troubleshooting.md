# Troubleshooting

Ordered roughly by how often each one catches people.

## No sound at all

Work down the chain rather than guessing.

**1. Is the firmware even running?** Watch the serial log. You should see
`audio_sink: ready: bck=4 ws=15 dout=2` at boot and `stream started` when the
phone plays. If `stream started` never appears, the problem is the Bluetooth
link, not the audio path — jump to the pairing section.

**2. Check the PCM5102A jumpers.** These are responsible for most silent
breakouts:

- **SCK floating** → silence. It must be tied to **GND** to select the internal
  PLL. This is the single most common cause.
- **XSMT low or floating** → soft-mute engaged. It must be tied to **3V3**.
- FLT, DEMP → GND for normal operation.

**3. Check the frame format.** `FMT` tied to GND is I²S (Philips), which is the
firmware default. If your breakout ties it high, switch to left-justified under
**BlueAudio Puck → audio output → I²S frame format**. A mismatch usually gives
distorted or channel-swapped audio rather than silence.

**4. Remember the output is line level.** The PCM5102A is not a headphone
driver. On 32 Ω headphones it is audible but quiet. Quiet is not broken.

**5. Confirm the GPIOs.** The defaults are BCK 4, LRCK 15, DIN 2. If you wired
differently, change them in `menuconfig` — the boot log prints what the firmware
is actually using:

```
I (603) audio_sink: ready: bck=4 ws=15 dout=2, 32 kB buffer, prefetch 25%
```

**6. Check the SCK bridge.** The direct-solder layout runs no MCLK wire, so the
`SCK` bridge on the front of the PCM5102A must be soldered. Without it the DAC
has no clock source at all.

## Distorted, crackly or channel-swapped audio

- **Swapped channels or a persistent buzz** usually means a frame-format
  mismatch. See point 3 above.
- **Regular clicks** suggest ring buffer underruns. Look for
  `underrun, re-prefetching` at debug level, and raise
  `PUCK_AUDIO_PREFETCH_PERCENT` or `PUCK_AUDIO_RINGBUF_KB`.
- **`ring buffer full, shedding audio`** means the source is pushing faster than
  the writer drains. Check that the writer task is on core 1
  (`PUCK_AUDIO_WRITER_CORE`) and not competing with the radio on core 0.
- **Slow pitch drift over a long track** points at clock accuracy. Confirm
  `PUCK_I2S_USE_APLL` is on — the default divider does not hit 44.1 kHz exactly.

## The puck does not appear in the pairing list

**This is usually correct behaviour.** Once the puck remembers a source it is
*connectable but not discoverable*, so known devices can return while strangers
cannot see it. The boot log tells you which state it is in:

```
I (1663) puck: 1 bonded source(s); hold the button to pair another
```

**Hold BT2 for 1.5 seconds** to open a 120-second pairing window. The LED blinks
fast while it is open, and the screen shows `PAIRING`.

To start completely fresh, **hold BT2 for 5 seconds** — that forgets every
bonded source and reopens pairing.

If it is genuinely never discoverable:

- Check `A2DP Enable with AVRC` appears at boot. `without AVRC` means AVRCP
  failed to initialise before the A2DP sink.
- Confirm the chip is an original ESP32. The banner logs
  `This chip has no Bluetooth Classic` on an S3 or C3, which cannot run this
  firmware at all.

## It pairs, then immediately disconnects

- Some phones drop an A2DP sink that never sends AVRCP responses. Check for
  `avrcp: AVRCP up` at boot.
- If the log shows repeated `A2DP event N dropped; link state may be stale`, the
  work queue is overflowing. That is a symptom worth reporting rather than
  tuning around.

## It goes to sleep on its own

Expected. `PUCK_IDLE_SLEEP_MINUTES` defaults to 15 minutes and counts only while
**nothing is connected** — a connected but paused source never triggers it.
Press the button to wake, or set the option to 0 to disable.

If it sleeps and wakes immediately, the wake pin is floating. `ext0` wakes on a
level and the button pulls its pin low, so the RTC pull-up must be enabled — the
firmware does this, but only for RTC-capable pins. GPIO 34–39 are input-only and
GPIO 33 is the tested default.

Note only **BT2** wakes the puck. `ext0` watches a single pin, so BT1 and BT3
do nothing while it is asleep — that is by design, not a fault.

## Build failures

**`fatal error: freertos/ringbuf.h: No such file or directory`** and similar —
a component is missing from `PRIV_REQUIRES` in its `CMakeLists.txt`. The IDF
error message names the component that provides the header.

**`'ret' undeclared`** inside an `ESP_GOTO_ON_*` macro — those macros report
through a local named `ret`. Declare `esp_err_t ret = ESP_OK;` in the function.

**`region 'iram0_0_seg' overflowed`** or an app that does not fit — confirm
`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`. The image is around 1.0 MB and the
default 1 MB factory partition does not fit it.

**`unknown kconfig symbol`** — the symbol does not exist in this IDF version.
`CONFIG_BT_SSP_ENABLED` is the trap here: it looks real because the IDF examples
define an *example-local* `CONFIG_EXAMPLE_SSP_ENABLED`, but SSP is a runtime
field (`esp_bluedroid_config_t.ssp_en`), not a build option.

**Whole-file diffs after editing** — Windows tooling wrote CRLF.
`.gitattributes` pins LF; run `git add --renormalize .` if it has already
happened.

## The screen is blank

The boot log answers this before you touch the wiring.

**Nothing at all in the log about the panel** — `PUCK_I2C_SDA_GPIO` or
`PUCK_I2C_SCL_GPIO` is `-1`, so the display is compiled out of the boot path.

**`no device at 0x3c on SDA=21 SCL=22`** — the driver probed and nothing
answered. Either the wiring is wrong, or the module is strapped to the other
address. SSD1306 boards ship at **0x3C or 0x3D** depending on a solder jumper;
try `PUCK_DISPLAY_I2C_ADDRESS=0x3D`. Check power and ground before the data
lines — a panel with no 3V3 obviously cannot ACK.

**`128x64 panel at 0x3c` but the screen stays dark** — the bus is fine and the
panel is answering. That points at the panel itself or its charge pump rather
than at wiring.

**The self test shows but text does not** — the bus, the panel and the driver
are all working, and the fault is in rendering. That is exactly why the self
test exists.

**Text appears but some glyphs are wrong** — that is the font table in
`components/puck_display/font5x7.c`, one five-byte row per character. It is in
its own file so a fix is a one-line edit.

**The screen is mirrored or upside down** — the segment remap (`0xA1`) and COM
scan direction (`0xC8`) in `ssd1306_init` set the orientation. Some modules are
built the other way round; swap them for `0xA0` and `0xC0`.

## A button does nothing, or does the wrong thing

The boot log lists every button it configured:

```
I (611) puck_ui: BT1 button on GPIO 32
I (611) puck_ui: BT2 button on GPIO 33
I (621) puck_ui: BT3 button on GPIO 27
```

**"BT3 button not fitted"** — that pin is set to `-1` in `menuconfig`.

**Nothing at all happens** — the firmware logs every press at info level
(`BT1 tap`, `BT2 hold`). If pressing produces no line, the switch is not
reaching the pin, or it is wired to 3V3 instead of GND. These are active low
with internal pull-ups: the switch belongs between the pin and **ground**.

**A tap registers as a hold** — the contact is staying closed, or the pin is
shorted low. Check the log timestamps: a tap and its release should be tens of
milliseconds apart.

**A hold never repeats** — only BT1 and BT3 repeat. BT2 fires once at 1.5 s and
again at 5 s, deliberately: nobody wants pairing mode to retrigger while their
thumb rests on the button.

## The signal bars are always full

**That is almost certainly correct.** Bluetooth Classic reports RSSI as a delta
from the Golden Receive Power Range, not as absolute strength, and zero — a
healthy link — maps to four bars. The value only goes negative once the link is
actually struggling, so on a desk next to the phone the meter should be full.

To see it move, walk away from the source until audio starts breaking up.

**The bars never appear at all** — they are drawn only while a source is
connected. On the pairing and idle screens there is nothing to measure.

**The bars show only their baselines** — a reading has not come back yet, or
the last read failed. The firmware invalidates the meter rather than showing a
stale value, so this is what a link in trouble looks like. `PUCK_RSSI_POLL_SECONDS`
sets how often it asks.

## The battery reading looks wrong

**It says `USB` but a cell is connected** — `PUCK_BATTERY_ADC_GPIO` is `-1`, so
the firmware never looks at the pin. The boot log says so plainly:

```
I (...) puck_batt: no battery sense fitted; assuming USB power
```

Set it to the divider pin (35 on the reference board). A working divider reads
about half the cell voltage: measure the midpoint with a meter and expect
~1.9 V for a cell around 3.8 V.

**It says `USB` and nothing is connected** — correct. That is the "not fitted"
report, not a failure.

**`implausible cell voltage ... check the divider resistors`** — the reading is
outside 2.5–4.6 V, so it is not a battery. Either the divider ratio does not
match the resistors actually fitted, or the ADC pin has nothing on it and is
reading noise.

**The percentage is nonsense** — check `PUCK_BATTERY_DIVIDER_RATIO_X100` against
the resistors you actually fitted. Two equal resistors are `200`. A wrong ratio
scales every reading.

**`no ADC calibration on this chip`** — that ESP32 has no eFuse calibration
data. Readings fall back to a nominal scaling and are approximate; nothing is
broken.

**It moves around during playback** — expected, and partly real. Li-po voltage
sags under load, and this device draws around 150 mA while streaming. The
reading is smoothed but the underlying sag is genuine. A voltage-derived
percentage is an estimate, not a fuel gauge.

## Flashing and monitoring

**`Wrong boot mode detected (0x13)! The chip needs to be in download mode.`** —
the board did not auto-reset into the bootloader. **Hold the BOOT button** as
the flash starts and release it once it begins writing. Some devkits never
auto-reset reliably, and a DAC loading GPIO 2 makes it worse, since that line is
both I²S data and a strapping pin.

**`Could not open COMn, the port is busy`** — something else holds it. A VS Code
serial monitor, a previous `idf.py monitor`, or a script that did not close its
handle. Close them and retry.

**The port number changed** — a different USB-serial chip enumerates as a
different port. Check which is present rather than assuming:

```powershell
[System.IO.Ports.SerialPort]::getportnames()
```

**`idf.py monitor` hangs a non-interactive shell.** It is interactive by design.
To capture a log from a script, open the port directly and pulse RTS to reset:

```powershell
$p = New-Object System.IO.Ports.SerialPort COM10,115200,None,8,one
$p.Open(); $p.DtrEnable = $false; $p.RtsEnable = $true
Start-Sleep -Milliseconds 200; $p.DiscardInBuffer(); $p.RtsEnable = $false
Start-Sleep -Seconds 5
$p.ReadExisting(); $p.Close()
```

Replace `COM10` with whatever the port scan above reports.

## Boot loops

Read the panic line. `Guru Meditation Error: Core 0 panic'ed (LoadProhibited)`
is a null or bad pointer dereference.

One found in this project, worth knowing: **`esp_a2d_sink_disconnect(NULL)`
panics.** It takes the address by value as an array parameter, so `NULL` is read
straight through. Always guard on an actually-connected peer.

To recover a bricked-looking board, hold GPIO 0 low during reset to enter the
serial bootloader, then flash normally.

## Measuring what is actually happening

**What is on the I²C bus.** The display driver probes its configured address at
start-up and logs the result either way, so the boot log already tells you
whether the panel answered.

**EQ cost.** Enable `PUCK_EQ_BENCHMARK_AT_BOOT` and read the log — it reports
microseconds per block and the share of one core, with every band forced active.
No DAC or audio source needed. Turn it back off afterwards.

**Buffer health.** `audio_sink_get_stats()` returns packets accepted, payloads
dropped, underruns and current buffer occupancy. The counters are the reliable
signal; the logs on those paths are deliberately rate-limited.

**Power.** Do not bother measuring on a devkit — the CP210x bridge and the
AMS1117 regulator dominate its draw whatever the firmware does. Meaningful
numbers need a bare module on your own board.
