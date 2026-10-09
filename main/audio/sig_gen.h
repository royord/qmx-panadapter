#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- Tab5-side I/Q test-tone generator --------------------------------------
//
// A bench signal on the ES8388 headphone/speaker out: a pure tone whose two
// stereo channels are in QUADRATURE - L carries I = cos, R carries Q = sin,
// 90 degrees apart - i.e. the same relationship a real I/Q feed has between
// its two wires. Playing it from the Tab5 itself exercises the whole output
// chain (and, with a stereo receiver at the other end, the operator's cabling)
// with no radio in the loop. See memory/project_kx3_support.md open item 3.
//
// STATE IS RAM-ONLY BY DESIGN. It starts OFF on every boot and neither this
// module nor any of its callers writes NVS - "never survives a reboot" is an
// explicit requirement, and the reason a unit left running on the bench is
// silent after a power cycle.
//
// Rendering belongs to the RX-audio task: rx_audio.c owns the codec and is
// the only I2S writer in the firmware. This module only fills buffers and
// keeps the on/off request flag. The off switch itself is
// rx_audio_siggen_set(false) (rx_audio.h) - it always succeeds, from any
// state, with no preconditions.

#define SIG_GEN_DEFAULT_HZ 1000.0f

// Operator request. Written by the UI task, read by the audio task; a single
// bool store is atomic on this core pair, and a one-frame stale read costs
// at most 21 ms of extra tone.
void  sig_gen_set_enabled(bool on);
bool  sig_gen_enabled(void);

// Tone frequency, clamped to 100 Hz .. 8 kHz (DC is not a test tone; above
// Nyquist/2 would alias). Takes effect on the next frame - the phase is
// continuous across a change, so retuning never clicks. RAM-only, no UI
// exposed for it yet: the drawer switches the generator on/off only.
void  sig_gen_set_freq_hz(float hz);
float sig_gen_freq_hz(void);

// Audio-task side: true while ANY output remains - the request is still on,
// or the stop-fade is still ramping down. The rx_audio task loop uses this
// to decide it may go back to sleep.
bool sig_gen_running(void);

// Fill `pairs` interleaved L/R frames at DSP_SAMPLE_RATE_HZ (48 kHz), with a
// ~10 ms fade in/out driven by the request flag so start and stop are never
// a step discontinuity (a click through the output stage). Returns the same
// as sig_gen_running() after rendering: false means the fade is done and the
// caller should leave the generator branch.
bool sig_gen_render(int16_t *stereo, int pairs);
