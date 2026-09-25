#pragma once
/* Radio profile — which transceiver drives CAT and where the I/Q samples come
 * from.
 *
 * The QMX is a single USB composite: CAT (CDC) + I/Q (UAC) on ONE plug, and
 * its I/Q must be explicitly enabled via the Q9; CAT handshake.
 *
 * The Elecraft KX3 is different twice over:
 *   - its I/Q is ANALOG ONLY (the "RX I/Q" 2.5 mm jack — the KX3 owner's
 *     manual "Quadrature Outputs (RX I/Q)");
 *   - so the tab's USB host carries CAT on the KX3's own USB CDC, while the
 *     I/Q arrives from a USB line-in UAC dongle plugged into that jack.
 * A KX3 therefore means TWO USB devices on the host port (via a hub), and the
 * panadapter's audio source is the dongle, NOT the radio.
 *
 * The KX3's USB VID/PID could not be established from documentation during
 * bring-up (searches 2026-09-22 returned nothing authoritative), so both the
 * KX3's and the dongle's identities are NVS-configurable. Default 0/0 means
 * "unknown": the CAT opener and the audio opener then refuse loudly and name
 * the setting to change. Read the values from the vendor's USB report / the
 * host PC's Device Manager / the bus-dump lines logged by audio.c, and set
 * them via radio_set_*() (a debug hook; UI comes later).
 */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_QMX = 0,   /* default: single composite USB, CAT + I/Q on one device */
    RADIO_KX3 = 1,   /* KX3: CAT on its own CDC; I/Q from a line-in UAC dongle */
} radio_kind_t;

/* Load the profile from NVS (idempotent). Call once at boot, before the CAT
 * link task and the audio task start. */
void radio_init(void);

radio_kind_t radio_get_kind(void);
const char   *radio_kind_name(void);   /* "QMX" / "KX3", for logs */

/* CAT (CDC-ACM) device to open. Returns 0/0 when the radio's identity is not
 * known yet (KX3 VID/PID unset). */
void radio_cat_vid_pid(uint16_t *vid, uint16_t *pid);

/* The UAC device that carries I/Q samples. For the QMX this IS the radio's
 * own device; for the KX3 it is the line-in dongle. Returns 0/0 when the I/Q
 * source is not known yet. */
void radio_iq_vid_pid(uint16_t *vid, uint16_t *pid);

/* True (QMX) when the radio streams I/Q over its own USB and CAT owns the
 * IQ-mode handshake (Q9;). False (KX3) means the panadapter's audio comes
 * from the dongle and no CAT handshake exists: nothing to enable, nothing to
 * confirm, nothing to re-assert. */
bool radio_iq_over_usb(void);

/* I/Q channel order. The QMX's UAC stream sends I on the left sample and Q on
 * the right. A dongle (or the KX3's I/Q jack) may come up mirrored — which
 * shows as an inverted spectrum with collapsed image rejection. Flip this and
 * the decode swaps the channels. Default off; set at bring-up if the spectrum
 * is mirrored. */
bool radio_iq_swap(void);
void radio_set_iq_swap(bool swap);

/* NVS setters for bring-up. Persist immediately. */
void radio_set_kind(radio_kind_t kind);
void radio_set_kx3_vid_pid(uint16_t vid, uint16_t pid);
void radio_set_dongle_vid_pid(uint16_t vid, uint16_t pid);

/* CAT auto-detect (first-plug, zero-NVS) - protocol probe, NOT vid/pid.
 *
 * Long-standing pain (2026-09-15, "I don't understand nvs set or vid/pid... can
 * we detect this until seen or have a menu to select the radio to connect
 * to?"): the very first KX3 plug refused to open because the CAT identity
 * (VID/PID) had to be typed/pinned first imposing a mennonite ceremony before
 * anything would work. That was wrong in principle: the panadapter OPENED the
 * QMX by hardcoded 0483:A34C, and now opens the I/Q side by pure class
 * (audio.c:694, mirroring the dongle self-find) - only CAT demanded an identity
 * before it would try.
 *
 * Now: CAT opens by PROTOCOL PROBE, classifying the radio from what answers
 * FA;/VN; - QMX (composite 0483:A34C) vs any Kenwood-KX3 CAT CDC. The probe
 * BOTH:
 *   - classifies the radio kind (QMX vs KX3) automatically on first plug, and
 *   - learns and persists the vid/pid into NVS for next-boot speed.
 * So the user never needs NVS set vid/pid: the radio "until seen" then
 * remembered. radio_set_kind()/radio_set_kx3_vid_pid() remain as overrides.
 *
 * -----------------------------------------------------------------------------
 * ⛔ BUILD ISOLATION 2026-09-23: the ONE primitive below that needs a build-time
 * confirmation is `usb_host_get_device_descriptor()` (acked in audio.c via
 * uac_host_device_open(addr-less), and here via the USB-Host device handle the
 * CDC new-dev callback hands us). If the esp-usb CDC-ACM component in your
 * managed_components version differs, this is the seam to fix - everything else
 * in this probe uses only primitives already proven in-tree (cdc_acm_host_open,
 * data_tx_blocking, the -extra-port probe helpers in cat.c). */
void radio_note_cat_found(uint16_t vid, uint16_t pid); /* probe found a radio's CAT: classify + persist */

/* KX3 CAT baud. The QMX is fixed at 38400 (its firmware only speaks that); the
 * KX3 auto-detects baud from its first valid command, so this value only
 * matters for what the link and the probe try FIRST. Persists immediately;
 * unset defaults to 38400. A wrong value self-heals: the probe falls through to
 * the remaining bauds, and radio_get_cat_baud() never returns anything but
 * 38400 while kind == RADIO_QMX. */
bool     radio_cat_baud_valid(uint32_t baud);          /* 4800/9600/19200/38400 */
uint32_t radio_get_cat_baud(void);                     /* policy: 38400 for QMX, else stored */
void     radio_set_cat_baud(uint32_t baud);

#ifdef __cplusplus
}
#endif
