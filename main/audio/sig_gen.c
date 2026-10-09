// I/Q test-tone generator - see sig_gen.h for what this is for and why the
// state is RAM-only. Everything here runs in ONE task (rx_audio_task): the UI
// only ever touches s_req/s_freq_hz, both single-word stores.

#include "sig_gen.h"

#include <math.h>

#include "dsp.h"          // DSP_SAMPLE_RATE_HZ

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// -9 dBFS peak per channel. Deliberate headroom: the normal RX path is bounded
// by out_clamp after its AGC, and this tone bypasses all of that - at full
// codec volume it must still sit below digital full scale rather than being
// the one thing in the firmware that can slam the output stage.
#define SIG_GEN_AMP 0.35f

// 10 ms linear fade in/out (48 kHz) - the same length the RX path uses for its
// own gap fades (rx_audio.c), long enough to be inaudible as a ramp, short
// enough to fit inside a single 1024-pair frame.
#define SIG_GEN_FADE_SAMPLES (DSP_SAMPLE_RATE_HZ / 100)

// ---- state ------------------------------------------------------------------
// s_req/s_freq_hz: written by the UI task, read here. Volatile because the
// readers are in a loop the compiler cannot see across tasks.
// s_phase/s_amp: audio-task only, never touched from anywhere else.
static volatile bool  s_req     = false;
static volatile float s_freq_hz = SIG_GEN_DEFAULT_HZ;
static float          s_phase   = 0.0f;   // radians
static float          s_amp     = 0.0f;   // 0..1 fade envelope

void  sig_gen_set_enabled(bool on) { s_req = on; }
bool  sig_gen_enabled(void)        { return s_req; }

void sig_gen_set_freq_hz(float hz)
{
    if (hz < 100.0f)  hz = 100.0f;
    if (hz > 8000.0f) hz = 8000.0f;
    s_freq_hz = hz;
}

float sig_gen_freq_hz(void) { return s_freq_hz; }

bool sig_gen_running(void) { return s_req || s_amp > 0.0f; }

bool sig_gen_render(int16_t *stereo, int pairs)
{
    // Snapshot once per frame: a mid-frame frequency change would otherwise
    // mix step sizes (still clickless thanks to the phase continuity, but a
    // pointless special case - 21 ms of latency on a bench tool is nothing).
    const float freq  = s_freq_hz;
    const float step  = 2.0f * (float)M_PI * freq / (float)DSP_SAMPLE_RATE_HZ;
    const float ramp  = 1.0f / (float)SIG_GEN_FADE_SAMPLES;
    const float target = s_req ? 1.0f : 0.0f;
    float phase = s_phase;
    float amp   = s_amp;

    for (int i = 0; i < pairs; i++) {
        // Envelope chases the request at a fixed slope: ±ramp per sample is
        // exactly SIG_GEN_FADE_SAMPLES (10 ms) end to end. On stop, the tail
        // of this frame ramps to true zero, so the codec never sees a step.
        if (amp < target)      { amp += ramp; if (amp > target) amp = target; }
        else if (amp > target) { amp -= ramp; if (amp < target) amp = target; }

        const float a = amp * SIG_GEN_AMP;
        // The quadrature pair itself: L = I = cos(phase), R = Q = sin(phase).
        // Two trig calls per sample is ~100k/s - trivially inside the 21.3 ms
        // frame budget, and it buys the cleanest possible definition of the
        // signal (no wavetable quantisation to explain away when someone is
        // using this to judge an audio path).
        const float l = cosf(phase) * a;
        const float r = sinf(phase) * a;

        phase += step;
        if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;

        stereo[2 * i]     = (int16_t)(l * 32767.0f);
        stereo[2 * i + 1] = (int16_t)(r * 32767.0f);
    }

    s_phase = phase;
    s_amp   = amp;
    return s_req || s_amp > 0.0f;
}
