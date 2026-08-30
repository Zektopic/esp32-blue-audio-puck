# Security model

What this firmware defends against, what it deliberately does not, and why.

The device is a battery-powered Bluetooth audio receiver. It holds no accounts,
no payment data and no user files. The realistic harms are **denial of service**
(a source that can wedge or flatten the puck), **unwanted access** (a stranger
pairing with it), and **impersonation** (someone who recovers a link key
pretending to be your puck).

## Pairing: Just Works, and why that is the honest choice

The puck declares IO capability `NONE`. It has no display and no keypad, so it
cannot show a passkey or accept one. Secure Simple Pairing therefore selects
**Just Works**: the link is encrypted, but there is no man-in-the-middle
protection and the resulting link key is *unauthenticated*.

Claiming `DisplayYesNo` instead would make the phone render a six-digit code
that nobody can compare against anything — worse than no claim, because it
looks like verification and is not. MITM resistance cannot be bought without an
I/O channel or an out-of-band pairing method, and this hardware has neither.

What *is* within our control is **when** pairing is possible:

- A puck with no bonded sources is discoverable from boot. It has to be, or it
  could never be paired at all.
- Once it remembers a source it is **connectable but not discoverable**. Known
  devices can reconnect; strangers cannot see it.
- A long press opens a bounded pairing window (`CONFIG_PUCK_PAIRING_WINDOW_SECONDS`,
  default 120 s), after which it closes itself.
- A five-second press forgets every bonded source and opens a fresh window.
  This is the only revocation a device with one button can offer, and without
  it there would be no way to remove a source that should no longer have access.

The window matters because Just Works pairing needs no user confirmation at
this end. A permanently discoverable puck can be bonded by anyone in range, at
any moment, with no interaction and no indication.

## Remote input is untrusted

Everything the source sends is treated as hostile:

- **AVRCP metadata** (`attr_text`) is not NUL-terminated and its length is
  chosen by the remote device. It is clamped both when copied onto the work
  queue and when stored, and it is printed to the console only after
  non-printable bytes are replaced — otherwise a source could inject ANSI
  escape sequences into the log of whoever is debugging the puck.
- **SBC codec configuration** falls through to a safe default on unrecognised
  capability bits, and the sample rate and channel count are validated again in
  `audio_sink` before they reach the I²S driver.
- **Absolute volume commands** are accepted only from the device that holds the
  audio stream. The command carries no address of its own, so the AVRCP target
  link's peer is compared against the A2DP peer.

## Denial of service

The work queue between Bluetooth stack context and the application task is the
pressure point, because a source can emit metadata responses far faster than
they can be handled.

- `bt_core_dispatch()` **never blocks**. A blocking enqueue would let a source
  that floods events stall the Bluedroid task, costing media packets and
  eventually the link.
- Every disposal path — handler completion, a full queue, shutdown — runs the
  destructor hook, so a dropped message cannot leak its deep-copied payload.
  Without this, a flood of metadata leaks heap without bound.
- Logs on saturated paths are rate limited. They are blocking UART writes, and
  they fire hardest exactly when the system is already struggling.

## Accepted risks

**Link keys are stored in plaintext NVS.** Neither flash encryption nor secure
boot is enabled. Roughly thirty seconds of physical access and a USB-serial
adapter yields the link key of every phone that has paired — enough to
impersonate the puck to that phone.

This is accepted for a DIY build. Before anything ships as a product, enable
flash encryption in release mode, NVS encryption and secure boot v2. That
decision has to be made *before* units are flashed: it permanently changes the
flashing workflow and cannot be applied retroactively.

**No firmware signing or OTA.** Anyone with physical access and a serial cable
can reflash the device. Same trade, same threshold: it matters when units leave
your desk.

## A note on SSP configuration

Secure Simple Pairing has **no Kconfig symbol** in ESP-IDF v5.5.4. The
`CONFIG_EXAMPLE_SSP_ENABLED` that appears in the IDF Bluetooth examples is
example-local, not a stack option — a plausible-looking trap if you go looking
for it in `sdkconfig`. SSP is selected at runtime through
`esp_bluedroid_config_t.ssp_en`, which defaults on and which `bt_core` now sets
explicitly rather than inheriting.

## Reporting

This is a personal project. Open an issue.
