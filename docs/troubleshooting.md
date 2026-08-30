# Troubleshooting

Ordered roughly by how often each one catches people.

## No sound at all

Work down the chain rather than guessing.

**1. Is the firmware even running?** Watch the serial log. You should see
`audio_sink: ready: bck=26 ws=25 dout=22` at boot and `stream started` when the
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

**5. Confirm the GPIOs.** The defaults are BCK 26, LRCK 25, DIN 22. If you wired
differently, change them in `menuconfig` — the boot log prints what the firmware
is actually using.

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

**Hold the button for 1.5 seconds** to open a 120-second pairing window. The LED
blinks fast while it is open.

To start completely fresh, **hold for 5 seconds** — that forgets every bonded
source and reopens pairing.

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

## Flashing and monitoring

**`Could not open COM10, the port is busy`** — something else holds it. A VS
Code serial monitor, a previous `idf.py monitor`, or a script that did not close
its handle. Close them and retry.

**`idf.py monitor` hangs a non-interactive shell.** It is interactive by design.
To capture a log from a script, open the port directly and pulse RTS to reset:

```powershell
$p = New-Object System.IO.Ports.SerialPort COM10,115200,None,8,one
$p.Open(); $p.DtrEnable = $false; $p.RtsEnable = $true
Start-Sleep -Milliseconds 200; $p.DiscardInBuffer(); $p.RtsEnable = $false
Start-Sleep -Seconds 5
$p.ReadExisting(); $p.Close()
```

## Boot loops

Read the panic line. `Guru Meditation Error: Core 0 panic'ed (LoadProhibited)`
is a null or bad pointer dereference.

One found in this project, worth knowing: **`esp_a2d_sink_disconnect(NULL)`
panics.** It takes the address by value as an array parameter, so `NULL` is read
straight through. Always guard on an actually-connected peer.

To recover a bricked-looking board, hold GPIO 0 low during reset to enter the
serial bootloader, then flash normally.

## Measuring what is actually happening

**EQ cost.** Enable `PUCK_EQ_BENCHMARK_AT_BOOT` and read the log — it reports
microseconds per block and the share of one core, with every band forced active.
No DAC or audio source needed. Turn it back off afterwards.

**Buffer health.** `audio_sink_get_stats()` returns packets accepted, payloads
dropped, underruns and current buffer occupancy. The counters are the reliable
signal; the logs on those paths are deliberately rate-limited.

**Power.** Do not bother measuring on a devkit — the CP210x bridge and the
AMS1117 regulator dominate its draw whatever the firmware does. Meaningful
numbers need a bare module on your own board.
