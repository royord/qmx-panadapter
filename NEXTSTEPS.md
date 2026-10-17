# NEXT STEPS — bench checklist

Branch `kx3`. Written 2026-10-17, after the dial-centred waterfall fix and the
README correction. Work in this order — the first test is the one everything else
depends on, and item 2 needs **preparation before you leave**.

---

## Before you power up

**Bring a beacon, or any signal on a frequency you know exactly.**

Item 2 cannot be done without one. Everything else can.

---

## 1. The waterfall now follows the dial — confirm it

This is the fix. The tuned-frequency line must line up with the content.

| Do this | Expect |
|---|---|
| Tune the radio's dial up a few kHz | The waterfall **scrolls** left/right; the tuned-frequency line stays in the middle of the screen and the signal you were looking at slides across it |
| Drag on the spectrum and lift | The radio retunes to the frequency under your finger; the view re-centres on it |
| Watch a signal while tuning towards it | It stays over your cursor rather than drifting away — this is the "latent smear" that was fixed |

**If the waterfall does not move at all**, say so — do not tune around it. That
would mean the tracking is not running, which the log will show.

To confirm from the serial log rather than by eye:

```
wf reframe: dx=58 moved=0 blanked=58 cols in 1097 us
```

You want a `wf reframe` line within a second or so of each `cat: Freq = ...`
line. `moved=0` is the **good** case — the canvas has spare margin so the window
slides instead of copying the image.

### Known and expected

- **The dial drifts.** On the KX3 the VFO walks (~200 Hz/s was observed). So you
  will see reframes trickling in even when you are not touching anything. Not a
  bug. It also means you cannot watch the frequency readout for long and expect
  it to hold still.
- **Columns entering the view go black.** That is correct — there is no history
  yet for frequencies that just came into view.

---

## 2. Measure the KX3's I/Q offset — the big remaining item

The KX3's I/Q centre is measured **~4 kHz off the dial**, so the tuned-frequency
line and a real signal do not quite agree. Nothing in the firmware can correct it
today: `ui_get_if_offset_hz()` hard-codes `0` for the KX3.

**Do this per mode**, because the offset is probably mode-dependent — in CW it
becomes the pitch offset, and USB/LSB may flip sign:

1. Put a **known-frequency** signal into the KX3 RX I/Q jack.
2. Tune the radio so the signal is comfortably in view.
3. Note where the signal sits relative to the tuned-frequency line. **+4 kHz to
   the right** or **−4 kHz to the left**? Write down which.
4. Repeat in CW, then USB, then LSB.

What I need from you: the sign, the magnitude, and whether it moves between
modes. That is enough to build the fix.

**Re-measure rather than trusting the old 4 kHz number.** Part of it may have
been the mode-table bug — the radio was reporting CW as FM, which put the wrong
LO offset in play. The table is fixed now (`MD3` correctly reads `CW (raw 3)`), so
the old figure may be stale.

⚠ **The I/Q test tone cannot calibrate this.** It loops through the Tab5's own
audio out and never touches the KX3's I/Q geometry.

---

## 3. I/Q orientation — is I on the left and Q on the right?

There is currently **no control for this at all**. `radio_set_iq_swap()` has no
caller anywhere — no UI, no web endpoint, no API — even though a comment tells
you to set it at bring-up. If your dongle is wired the other way round, nothing
you can do in the firmware will fix it today.

Use the **I/Q test tone** (settings drawer → I/Q test tone) as the reference. It
puts a known +1 kHz quadrature tone on the Tab5's own audio out. Compare that
against the KX3 path:

| What you see | What it means |
|---|---|
| Tone at **+1 kHz** from centre | Correct |
| Tone at **−1 kHz** | I and Q are **swapped** |
| Tone at +1 kHz but **nothing decodes** | Q is **inverted** |

Report which. Building the control is straightforward once we know it's needed.

---

## 4. CAT checks on a live radio

Already known good — worth a quick pass, not a deep investigation:

- `FA` / `MD` / `FR` all answer. `Mode = CW (raw 3)` in the log proves the
  corrected mode table.
- **VFO-B watchdog.** This one does **not** follow VFO B — it takes the radio back
  to VFO A, because every frequency commit this firmware makes (`FA`) addresses
  VFO A only. Switch the KX3 to VFO B and tune it. Within ~3 s you should see
  `KX3 VFO A watchdog: radio receiving on VFO mode 1 - FR0; ...` and the radio
  should return to VFO A, after which touch-to-tune is audible again. That
  warning line is **normal operation, not a fault**. It deliberately does *not*
  clear split (`FT`/`SP`) — if you run your own split, receiving on A and
  transmitting on B, it leaves that alone.
- **CAT baud picker.** If your setup is marginal, try 9600 / 38400 / 19200 / 4800
  in the drawer and note which works.

---

## 5. Quick sanity on the new readouts

- **CPU readout** in the bottom status bar: shows `CPU --/--` for the first 10 s
  after boot (the first measurement window has not closed — a confident `0/0`
  would read as "both cores idle", the opposite of the truth), then
  `CPU nn/nn%` updating once per new 10 s sample. On the KX3 expect roughly
  **80/30** — core 0 is much busier than core 1. Higher = busier here; the serial
  log reports the complement (idle %), which is easy to misread against it.
- **Banner topmost.** The amber tap-to-stop banner should stay above any prompt
  that comes up while it is showing.
- **Capture gain.** The log should show the dongle's volume being found and set
  to 100%. A faint bands/floor on the waterfall is the **true noise floor** now
  that the gain is correct, not a fault.

---

## Flash / reboot discipline

> **Every flash, and every reboot with a radio attached, needs a manual power
> cycle afterwards.**

- Flash **app only** at **`-b 230400`** to **`0x10000`** — keeps NVS (radio kind,
  CAT port, I/Q dongle, callsign), and 460800 drops the USB-Serial/JTAG link
  mid-write.
- Opening COM3 from a script **resets the device**. Use
  `tools/cap_serial_reboot.ps1 -Reset` to capture a clean boot; a plain capture
  often misses early lines to console re-enumeration.
- Power-cycle the radio after a flash **before** judging anything, or you will be
  diagnosing the previous build.

---

## If something fails

Two log lines carry most of the diagnosis:

```
wf reframe: dx=... moved=... blanked=... cols in ... us
```

```
still display: OFF - spectrum follows the dial
```

If either is missing or wrong, that is the finding — capture the log and send it
rather than working around it.