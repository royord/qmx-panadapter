# Flashing the KX3 / DigiRig Mobile firmware onto the Tab5

> **Status — read this first.** This build is **compile-verified but has never run
> on hardware.** It comes from an uncommitted development branch (`kx3`) whose own
> base commit is titled *"work in progress"*. Treat it as a bench test, not a
> release. Nothing here has been validated against a real KX3 or DigiRig Mobile,
> and the changes to CAT handling affect **QMX owners too** (see
> [step 10](#10-if-you-have-a-qmx-not-a-kx3)).

---

## 1. What this firmware adds

The released firmware treats the QMX as the only radio, because the QMX is the
only radio that puts I/Q on a USB UAC device. A KX3 has **no USB audio at all** —
so this branch adds:

- **CAT over a DigiRig Mobile.** A DigiRig Mobile is a two-cable-in-one box: a
  CP2102 serial bridge (CAT) and a CM108 UAC audio codec (I/Q) behind an internal
  USB hub. The firmware no longer looks for a QMX by VID/PID — it walks the USB
  bus and **identifies the radio by asking it** (`VN;` / `FA;`). A CP2102 needs
  no VID/PID configured, which is the point: a KX3's USB-serial chip is whatever
  the operator chose.
- **A 48 kHz sample-rate fix.** The DSP is pinned to 48,000 Hz and cannot
  resample. The old code took the *first* rate the descriptor happened to list,
  so a device offering 44.1 kHz first was driven at 44.1 kHz — everything 8.8%
  off, with a display that looked perfectly healthy. The rate is now selected
  explicitly, and a device that cannot do 48 kHz is **rejected with an error
  naming every rate it does offer**, rather than silently mistuned.
- **An I/Q dongle VID/PID picker.** Because there is no USB bus event to hang a
  discovery dialog on, you tell the firmware which UAC device is the I/Q source:
  **Settings → Radio → KX3 I/Q dongle**.

## 2. What you need

| | |
|---|---|
| **Radio** | KX3, or KX3+ |
| **Audio + CAT** | DigiRig Mobile (or any USB hub with a CDC-ACM serial bridge **and** a UAC audio device) |
| **Host** | M5Stack Tab5 (ESP32-P4) |
| **Flashing host** | A machine with the Tab5 plugged in. Windows is easiest. **WSL cannot flash** — see below. |
| **Flashing tool** | Python + `esptool`. A full ESP-IDF install is *not* required. |

### Why you cannot flash from WSL

The Linux-side build environment has no `/dev/ttyUSB*` or `/dev/ttyACM*` — no USB
serial device is passed through — and `WSLInterop` is unregistered, so no Windows
executable can be launched from it either. Flashing must happen on the Windows
side, or on the machine that physically holds the Tab5. WSL is for *building*.

## 3. Before you flash: the power-cycle rule

**Every flash with the radio attached wedges it. Always. No exceptions.**

This is not a firmware fault and no firmware change fixes it (issue #74). The
radio ends up in a state where it stops answering until a human unplugs and
re-plugs it. So:

1. Flash with the radio connected if you must, but
2. **Power-cycle the radio by hand immediately afterwards** — unplug the USB
   *and* the DC power, wait a few seconds, reconnect.

The firmware carries a watchdog for exactly this condition. If you later see
*"power-cycle the QMX"* on screen, that is the stale-signature detector reporting
a real wedge. It is not a new fault.

## 4. Get the firmware

The build output is on the WSL side, reachable from Windows Explorer as:

```
\\wsl.localhost\Ubuntu\home\royord\build\qmx-panadapter\build\flash_kx3\
```

| File | Bytes | Flash offset |
|---|---:|---|
| `bootloader.bin` | 22,800 | `0x2000` |
| `partition-table.bin` | 3,072 | `0x8000` |
| `qmx_panadapter.bin` | 4,598,528 | `0x10000` |
| `ota_data_initial.bin` | 8,192 | `0x920000` |

Also in that folder: `flash_kx3.bat` (a ready-made flash script) and
`flasher_args.json` (the exact parameters the build system used).

Chip **esp32p4**, flash **16 MB**, mode **dio**, **40 MHz**. The offsets above
are copied from the build's own `flash_args` file — if you ever rebuild, use the
new `flash_args`, not this table.

## 5. Find the COM port

Plug the Tab5 into the flashing host and note its COM port.

⚠ Every Tab5 enumerates as **`VID_303A&PID_1001` with no serial number**, so
Windows cannot tell your boards apart by ID — the COM port follows the *socket*,
not the board. Unplug and re-plug and watch which port disappears. If several
identical boards are attached, this is genuinely ambiguous; plug in one at a time.

## 6. Flash it

Either double-click `flash_kx3.bat` after editing its `PORT` line, or run this
from a shell in the `flash_kx3` folder:

```
python -m esptool --chip esp32p4 -p COM7 -b 460800 ^
  --before default_reset --after hard_reset ^
  write_flash --flash_mode dio --flash_freq 40m --flash_size 16MB ^
  0x2000 bootloader.bin ^
  0x8000 partition-table.bin ^
  0x10000 qmx_panadapter.bin ^
  0x920000 ota_data_initial.bin
```

Replace `COM7` with your port. A full image flash overwrites the partition table,
so **any saved settings are lost** — including the VID/PID you are about to enter,
and the callsign. Have the callsign and grid square ready to re-enter.

Then **power-cycle the radio** ([step 3](#3-before-you-flash-the-power-cycle-rule)).

## 7. Configure it for your KX3

Open the settings drawer (swipe or tap the drawer handle) → **Radio** group. Two
items are KX3-specific; they appear once the radio has been identified as a KX3.

### 7.1 KX3 I/Q dongle — **required**

Tap **KX3 I/Q dongle** and enter the USB VID:PID of the **UAC audio** device —
the CM108 inside the DigiRig Mobile. This is *not* the CP2102's VID:PID; the
CP2102 is only for CAT and is found by talking to it.

- Either hex digits with or without a `0x` prefix — `10C4`, `0x10C4`, `10c4` all
  work. 1 to 4 digits per field; a wider value is rejected rather than silently
  truncated.
- **Leaving both fields empty is a valid setting** meaning "not configured"
  (`0:0`). It is not an error, but it means no audio.

The dialog explains what the value is for: *a KX3 has no USB I/Q, so the firmware
needs to be told which UAC device to listen to.*

> **After saving, unplug and re-plug the dongle.** The firmware does not
> re-open the audio link on its own when you change this. It deliberately does
> not try to do a USB re-plug for you either, because that path would also drop
> the CAT link and has never been tested.

### 7.2 KX3 CAT baud — usually leave it alone

Tap **KX3 CAT baud** to step through **38400 → 9600 → 19200 → 4800**. 38400 is
the default and is what a KX3 normally uses. The change applies on the next link
re-open. Only change it if CAT is dead at every baud — see
[troubleshooting](#9-troubleshooting).

You do **not** select a radio type anywhere. Identification is automatic.

## 8. Verify

Open a serial monitor at **115200 baud** and look for these lines.

**The bus walk and the probe** — every attached device is logged, and new or
re-announced devices re-trigger the probe:

```
USB device at addr=1 is 303A:1001 (Tab5) - new link probe will re-run
USB device at addr=2 is 10C4:EA60 (CP2102 USB-UART) - new link probe will re-run
```

Then one of:

```
CAT probe: addr=2 if0 (10C4:EA60) identified as KX3
CAT probe: addr=2 if0 (10C4:EA60) answered no CAT at any baud
```

The second line means the port opened but the radio never spoke — go to
[troubleshooting](#9-troubleshooting).

**The audio side.** What you want to see is a stereo, 16-bit device at 48 kHz,
and the full list of rates the dongle offered:

```
Alt 1: UAC at addr=3 is 046D:0825 (HD Pro Webcam C270), NOT the I/Q source
```

That warning is expected if the Tab5's own webcam or another UAC device is
attached — it is being skipped, not failing. The device you configured should
appear as the selected source, and the rate list should include 48000.

## 9. Troubleshooting

**Silent spectrum, no audio.**
The log will say `I/Q source not configured (radio=..., VID/PID 0:0)`. Either the
dongle VID:PID was never set, or it was set and you did not re-plug the dongle.
Check both.

**`I/Q source offers no 48000 Hz rate ... It offers N: ...`**
A hard refusal, and a correct one. The list after `It offers` is every rate the
device offers; the DSP is pinned to 48 kHz and will not resample, so a dongle
that cannot do 48 kHz cannot be used. This is the failure that the old
first-rate-wins code used to hide.

**`offers a continuous 8000-48000 Hz range`**
The device supports a range rather than a fixed list, and 48 kHz is inside it.
This is handled correctly and is not a problem.

**Only one channel / mono.**
Mono is reported as a warning but I/Q de-interleaving genuinely cannot work —
there is no second channel to read the quadrature from. The CM108 in a DigiRig
Mobile is expected to enumerate as stereo; if it does not, check the dongle's
own configuration.

**CAT dead at every baud.**
Confirm the KX3's USB/CAT port electrical setting. The DigiRig Mobile ships in
*logic levels* mode, which is what a KX3 wants (3.3 V logic), and it also
supports switched RS-232 / CI-V / TX-500 — if it has been left in one of those,
CAT will never be found.

**The radio is dead after flashing.**
Expected. Power-cycle it. See [step 3](#3-before-you-flash-the-power-cycle-rule).

**Analog level doubts.**
Firmware cannot fix this. The KX3's RX I/Q output goes into the CM108's
*microphone* input, and the two levels may not match. This needs a bench and
possibly an attenuator.

## 10. If you have a QMX, not a KX3

The CAT changes are **not** KX3-only. The port-opening code was rewritten to walk
the bus instead of opening a known VID/PID. For a QMX with a known VID/PID it
should still take the direct path and behave as before, but that has not been
confirmed on hardware.

So flashing this onto a working QMX setup is a real behaviour change, not a
no-op. Do it on a bench where you can recover.

## 11. Rebuilding it yourself

Build on the Linux side; flash on the Windows side. The pipeline is:

```bash
cd /home/royord/build/qmx-panadapter
source /home/royord/esp/esp-idf/export.sh      # ESP-IDF v5.4.4
python3 tools/check_patches.py                 # refuse to build unpatched
idf.py build
```

Two rules that are easy to get wrong:

- **`fullclean` wipes `managed_components/`**, which is where several standing
  patches live. After a `fullclean` you must `idf.py reconfigure` to refetch,
  then re-apply **every** `tools/patches/apply_*.ps1`, then run
  `check_patches.py`. Building straight after a clean can produce an image with
  patches silently missing.
- **Apply patches one at a time in a loop that reports failures.** The documented
  one-liner using `ForEach-Object` aborts at the first failing script and skips
  the rest, which looks identical to success.

Verify what you are about to flash:

```bash
python3 tools/check_nul.sh          # no NUL bytes in the tree
python3 tools/check_patches.py      # all 24 patched sites present
```

If a clean build fails, read `build/log/idf_py_stdout_output_*` — idf.py's own
summary prints an unrelated `PRI` format-specifier hint that reads like the cause
but is not.

## 12. See also

- `docs/mkdocs/guide/audio.md` — the authoritative I/Q source guide
- `docs/mkdocs/reference/troubleshooting.md`
- `memory/project_kx3_support.md` — implementation detail and the reasoning
- `memory/project_bench_setup.md` — build environment
