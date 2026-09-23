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

#ifdef __cplusplus
}
#endif