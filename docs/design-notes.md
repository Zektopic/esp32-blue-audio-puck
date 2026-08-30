# Design notes

Why the firmware is shaped the way it is, and what went wrong on the way there.
This is the document to read before changing the audio path.

## The one rule

**Bluedroid callbacks only enqueue.**

`a2dp_data_cb` and every event callback run on Bluedroid's own task. Anything
slow there stalls the Bluetooth stack, which costs media packets, then link
supervision, then the connection. So:

- The A2DP data callback does one non-blocking `xRingbufferSend` and nothing else.
- Event callbacks do one non-blocking `bt_core_dispatch` and nothing else.
- Every cost — the blocking I²S write, the EQ, the volume gain — happens on the
  audio writer task, pinned to core 1, while the radio owns core 0.

Two consequences that look like over-engineering until you know the rule:

- `bt_core_dispatch` uses a **zero** queue timeout. Dropping work is the
  designed outcome; blocking would hand a misbehaving source a way to stall the
  stack 10 ms at a time.
- Logs on those paths are rate-limited. `ESP_LOGx` is a blocking UART write, and
  the paths that log most are the ones already under pressure.

## Why not reuse the IDF example's components

In ESP-IDF v5.5 the A2DP sink example was renamed `a2dp_sink` →
`a2dp_sink_stream` and its logic moved into shared components under
`examples/bluetooth/bluedroid/classic_bt/common/`, pulled in by `${IDF_PATH}`
paths in `main/idf_component.yml`.

Depending on those makes a repository unbuildable on any other machine or in CI.
So this firmware implements the same API surface itself — which had the side
effect of surfacing three defects that the example carries.

### Bug 1 — `i2s_channel_write(..., portMAX_DELAY)`

That parameter is **`timeout_ms`**, and the driver passes it to `pdMS_TO_TICKS`,
which is 32-bit. `portMAX_DELAY` overflows to roughly **twelve hours**.

It only bites around a stop. A disabled channel makes the write block on a
semaphore that only `i2s_channel_enable()` ever gives, so a writer that entered
the call just after `audio_sink_stop()` wedges — still holding a ring buffer
item. The window is roughly 0.2 ms of an 8.16 ms chunk period, and
`audio_sink_stop()` runs on every disconnect and every format change.

This firmware uses **100 ms**, about 18× the 5.44 ms descriptor period.

### Bug 2 — reconfiguring a live I²S channel

`i2s_channel_reconfig_std_clock` and `..._slot` assert
`state == I2S_CHAN_STATE_READY`, meaning the channel must be **disabled**. The
example calls them from its `AUDIO_CFG` handler while the channel is enabled and
ignores the return value, so the reconfiguration silently does nothing.

`audio_sink_set_format()` stops the stream, reconfigures, and restarts it.

### Bug 3 — deleting the writer task on disconnect

The example creates and `vTaskDelete`s its writer per connection, which can
delete a task in the middle of a write. Here the writer is created once at init
and parked on a task notification.

## The flush that silently did nothing

`audio_sink_stop()` drains the ring buffer so stale audio from the previous
stream does not play on resume — at the *new* sample rate, if the stop was for a
format change.

That drain was a no-op. FreeRTOS byte-mode ring buffers refuse a second
retrieval while one is outstanding (`prvCheckItemAvail` in `ringbuf.c`), so
draining while the writer held an item returned `NULL` immediately, freed
nothing, and reported no error.

The fix is the `parked` semaphore: the writer gives it before sleeping, and
`stop()` waits on it before touching the channel or the buffer.

## States with no wakeup source

The prefetch state machine had a subtle hole. The only `xTaskNotifyGive` that
started playback was on the successful *prefetch → playing* edge. Both
`audio_sink_start()` and the *dropping → playing* edge reached a runnable state
without notifying anyone.

The terminal sequence:

1. Buffer near full during normal playback.
2. Disconnect, or a 48 kHz reconfigure. The writer wedges (bug 1), and the
   flush no-ops.
3. `audio_sink_write` kept accepting — it never checked `running` — so the
   buffer hit 100% and entered drop mode.
4. On restart the writer parked. Drop mode's exit test asks whether the buffer
   has drained, which can never become true with a parked writer.
5. **Silence for the rest of the session.**

Fixed three independent ways: notify on both missing edges; refuse writes while
stopped, which removes the precondition entirely; and give the park a 100 ms
backstop so a missed edge is a hiccup rather than the end.

## Why the coefficients are double-buffered

A `biquad_coeffs_t` is five floats plus a flag. **No amount of `volatile` makes
a 24-byte struct update atomic.** The application task rewriting coefficients in
place while the writer reads them per sample can be observed half-published.

That is not a glitch that passes. A torn `(a1, a2)` pair can put the poles
outside the unit circle, and a recursive filter with unstable poles grows until
saturation turns it into a full-scale square wave — which **outlives** the
inconsistency, because nothing clears the state.

So: two coefficient sets, filled inactive-side and published by flipping an
index behind a release fence. The writer reads the index once per block. Filter
state is writer-owned and cleared through a `reset_pending` flag the writer
honours at block start, so the application task never touches it. Inner-loop
cost: zero.

The deterministic trigger was `puck_main.c`'s config handler, which restarts the
sink and only *then* recomputes coefficients — guaranteed on any 48 kHz source.

### Contrast: where `volatile` **is** enough

`running`, `mode`, `gain_q15`, `channels` and `processor` are all single-word
aligned accesses on an LX6 with no data cache on internal SRAM. Nothing can
tear. Every other defect in the audio path was *logical* — a missing wakeup
edge, a check-then-act window — not a memory-model problem.

## Coefficients are a function of the sample rate

Biquad coefficients depend on frequency ÷ sample rate. A source negotiating
48 kHz instead of 44.1 kHz shifts the entire EQ curve up by about 9%. So
`ESP_A2D_AUDIO_CFG_EVT` drives both `audio_sink_set_format()` and
`audio_dsp_set_sample_rate()`, and any rate change clears the filter state,
because state belonging to the old rate rings.

## How many biquads fit

The Hackaday DSP project this build draws on left that as an open question.
Measured on an ESP32-WROOM at 160 MHz with `PUCK_EQ_BENCHMARK_AT_BOOT`:

```
benchmark: 5 active section(s), 360 frames in 1337 us (8163 us of audio, 16% of one core)
```

**~3.2% of one core per stereo biquad section**, so roughly 30 sections before
one core is saturated — far more headroom than the usual assumption.

Two design choices follow from it:

- **Flat bands are excluded from the published set**, not flagged and skipped.
  A flat equaliser is the default; scanning bypass flags per sample to learn
  there is nothing to do is waste. With every band flat the pass returns on one
  comparison.
- **Live sections are copied to the stack once per block.** Read through the
  global they were reloaded through a pointer per sample per band.

Float was the right call. The ESP32 has a hardware FPU, the writer is pinned so
it effectively owns the coprocessor on its core, and the 96-byte context area is
reserved for every task regardless — so float costs the writer no extra stack.

## Latency and the prefetch cushion

Playback waits until the buffer is `PUCK_AUDIO_PREFETCH_PERCENT` full, then runs
until empty. That percentage is therefore also **what every underrun costs to
rebuild**.

At the 32 kB default, the original 60% was 111 ms. The DMA chain holds 33 ms and
keeps playing during a refill, so each underrun was an audible **78 ms gap** —
and 111 ms of added latency is enough to break lip-sync on video.

Now 25%, about 46 ms: still comfortably above the DMA depth and typical BR/EDR
retry windows.

## Frame alignment holds by arithmetic, not by construction

Interleaved 16-bit stereo frames are 4 bytes. Every A2DP PCM payload,
`RINGBUF_BYTES` (32768) and `WRITE_CHUNK_BYTES` (1440) are multiples of 4, so a
ring buffer wrap can never hand back a frame-straddling block. If any of the
three stopped being a multiple of 4, `audio_dsp_process` would silently swap
left and right for the rest of the stream — so there are `_Static_assert`s
documenting the invariant.

## Power: what firmware can and cannot reach

The ESP32 with Bluetooth Classic active is roughly three quarters of the current
budget. No firmware change closes the gap to a purpose-built Bluetooth audio SoC
— a Qudelix 5K manages ~20 hours because its SoC idles in single-digit
milliamps with hardware codec engines.

What firmware reaches: transmit power (the source is usually in the same
pocket), 80–160 MHz frequency scaling, and what the puck does with the hours it
spends in a bag.

**Light sleep stays off deliberately.** The controller keeps its reference clock
on the main crystal, which supports DFS but not light sleep, and light sleep
would break audio timing regardless.

**Sniff mode is not requested.** Bluedroid exposes `ESP_BT_PM_MD_SNIFF` only on
the mode-change *event* and offers no public call to request it — the controller
manages it internally. The firmware reports the link power mode rather than
claiming to control it.

**Deep sleep buys nothing during playback.** It is the difference between flat
by morning and fine next week.

## Things that bit during development

- **`esp_a2d_sink_disconnect(NULL)` panics.** It takes the address by value as
  an array parameter, so `NULL` is read straight through — `LoadProhibited`. It
  was harmless while pairing mode was only reachable from a button press with
  something connected; making boot enter pairing mode turned it into a boot
  loop. Always guard on an actual linked peer.
- **AVRCP metadata needs a deep copy *and* a matching free.** `attr_text` points
  into a buffer Bluedroid owns and reuses, and it is not NUL-terminated. A copy
  hook without a destructor leaks on every disposal path that skips the handler
  — which is exactly the path a burst of metadata drives.
- **SSP has no Kconfig symbol in v5.5.4.** The `CONFIG_EXAMPLE_SSP_ENABLED` in
  the IDF examples is example-local. SSP is `esp_bluedroid_config_t.ssp_en`.
- **Initialise AVRCP before `esp_a2d_sink_init()`**, or Bluedroid logs
  `A2DP Enable without AVRC` and builds an incomplete SDP record. Initialise it
  before the UI task too, or a button press can reach a mutex that does not
  exist yet.
- **Python on Windows writes CRLF by default.** Editing sources with it turns
  three-line changes into whole-file diffs. `.gitattributes` now pins LF.
