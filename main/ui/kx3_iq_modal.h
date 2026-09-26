#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the KX3 I/Q dongle modal at boot. Call once from ui_init() when
// internal heap is at its boot-time maximum, alongside the other modals - see
// identity_config_modal_init() for the rationale.
void kx3_iq_modal_init(void);

// Show the KX3 I/Q dongle modal as a full-screen overlay. Two hex fields, the
// USB VID and PID of the line-in dongle that carries the KX3's analog I/Q.
// Save persists via radio_set_dongle_vid_pid() (NVS). Cancel closes unchanged.
// Safe to call repeatedly; no-op if already open.
void kx3_iq_modal_show(void);

#ifdef __cplusplus
}
#endif
