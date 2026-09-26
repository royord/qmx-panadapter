#include "audio.h"
#include "cat.h"          // cat_is_ready - the dead-stream watchdog's CAT-alive test
#include "radio.h"        // which UAC device carries the I/Q stream (KX3: the dongle)
#include "usb_replug.h"   // the dead-stream watchdog's escalation
#include "util/usb_patch_counters.h"  // #189: report the silent USB patches' counts
#include "util/sock_probe.h"          // #313: socket-exhaustion canary
#include "dsp.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"  // MALLOC_CAP_SPIRAM for xRingbufferCreateWithCaps

#include "usb/uac_host.h"
#include "iq_balance.h"
#include "psram_task.h"
#include "ui.h"

static const char *TAG = "audio";

// Internal event types we put on our queue
typedef enum {
    AE_NONE = 0,
    AE_RX_CONNECTED,
    AE_RX_DONE,
    AE_DISCONNECTED,
    AE_TRANSFER_ERROR,
} audio_evt_kind_t;

typedef struct {
    audio_evt_kind_t kind;
    uint8_t addr;
    uint8_t iface_num;
} audio_evt_t;

#define EVT_QUEUE_LEN          16
#define RX_BUF_BYTES           19200
// UAC driver-internal ring: 288000 B = 1.0 s of 48 kHz stereo 24-bit audio
// (#51 fix, was 19200 B = 66 ms). The post-decode storm (LVGL decode-list
// rebuild + dual-core LDPC, PSRAM-bus heavy) slows the drain for hundreds of
// ms every slot; at 66 ms of elasticity the driver overflowed and silently
// discarded whole ISO transfers (~200-650 ms of RF audio per slot, measured
// sample-exact via capture-window tiling), which destroyed each FT8 signal's
// opening Costas array on every slot after the first. Buffer >16 KB allocates
// from PSRAM (spill threshold), so the 280 KB costs nothing internal.
#define INTERNAL_RX_BUF_BYTES  288000
#define STATS_PERIOD_MS        1000

// Ring buffer holds decoded int16 stereo pairs (4 bytes per pair).
// 64 kB / 4 = 16384 pairs = ~341 ms of headroom @ 48 kHz
#define SAMPLE_RING_BYTES      (64 * 1024)

// Producer-side state
static TaskHandle_t s_audio_task = NULL;
static QueueHandle_t s_evt_queue = NULL;
static uac_host_device_handle_t s_uac_dev = NULL;

// Partial-frame carry and its measurement. Declared here rather than beside
// process_rx() because the 1 Hz stats logger below reports the counters.
// Full reasoning lives at process_rx().
static uint8_t  s_carry[6];
static size_t   s_carry_len = 0;
static uint32_t s_misalign_events = 0;
static uint32_t s_misalign_bytes  = 0;

void audio_reset_frame_alignment(void)
{
    s_carry_len = 0;
}
static RingbufHandle_t s_ring = NULL;
static uint8_t *s_ring_storage = NULL;   // where the ring's 64 KB landed (PSRAM vs internal)

// Periodic heap watchdog: logs internal-DRAM free + largest contiguous block
// (what the SDIO/USB DMA paths actually need) plus the sample-ring address, so
// internal-RAM pressure is always visible on the serial console without needing
// QMX connected or an FT8 slot to complete. Fires every 10 s.
//
// v2 (2026-07-13, FT4 cyan-flash fix): O(1) queries ONLY on the periodic
// path. heap_caps_get_largest_free_block() walks every block of the internal
// heaps inside the heap spinlock — an interrupts-off critical section long
// enough to delay the core-0 MIPI-DSI frame-restart ISR and blank the panel
// for a frame (the FT4 "full-screen cyan flash"). Core-pinning does NOT
// contain it (hardware-verified: a core-0 heap op spins ints-off waiting for
// the same lock), so the walk itself has to go. free/min/psram-free are
// cached counters (O(1), tiny lock). The lblk walk runs ONLY when internal
// free has sunk below an emergency threshold — fragmentation forensics on a
// nearly-exhausted heap is worth a one-frame blink; steady state is not.
#define HEAP_WD_LBLK_EMERGENCY_BYTES (24 * 1024)
static void heap_watchdog_task(void *arg)
{
    (void)arg;
    int s_heap_quiet = 5;   // first healthy report after 10 s, then every 60 s
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        // Standing USB patches #7/#8 turn two aborts into survivable errors but
        // may not log from the interrupt path, so they count instead and this is
        // where the count is read (TODO #189). Prints only on change, so a
        // healthy device stays quiet and the line appearing IS the evidence.
        usb_patch_counters_report();
        /* #313: the socket table filling up is silent - two listeners stop
         * accepting while ping, CAT and FT8 all keep working. One socket()
         * plus close(), change-detected, so a healthy device stays quiet. */
        sock_probe_report();

        size_t i_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t i_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t p_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (i_free < HEAP_WD_LBLK_EMERGENCY_BYTES) {
            size_t i_lblk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            ESP_LOGW(TAG, "HEAP: int free=%uKB (min=%uKB lblk=%uKB LOW!)  psram free=%uKB  ring@%p[%s]",
                     (unsigned)(i_free / 1024), (unsigned)(i_min / 1024), (unsigned)(i_lblk / 1024),
                     (unsigned)(p_free / 1024), (void *)s_ring_storage,
                     (((uintptr_t)s_ring_storage >> 24) == 0x4F) ? "INTERNAL" : "PSRAM");
        } else if (++s_heap_quiet >= 6) {
            // Every 60 s when healthy (2026-09-13 log audit: it was 6 W lines a
            // minute). The LOW branch above still reports every 10 s.
            s_heap_quiet = 0;
            // TEMP DIAGNOSTIC (2026-07-26): dma= tracks the MALLOC_CAP_DMA pool.
            // SD failures show it exhausted (704 B largest block) while general
            // internal RAM is fine (31 KB largest) - this time series shows WHEN
            // it drains. get_free_size is an O(1) counter query so it is safe
            // here; largest_free_block walks the heap with interrupts off and
            // must NEVER go on this periodic path (that caused the FT4 cyan
            // flash), which is why only "free" is reported.
            /* #313: sock= is the LWIP socket headroom, CACHED so this 10 s
               line costs no real probe of its own (sock_probe.h explains why
               the uncached call must not go on a polled path). It is here so a
               SOAK captures the trend - the wedge took 10 h to appear and the
               only reading anyone had was after the fact. A steady figure that
               walks down over hours is the leak; a steady one is not. */
            ESP_LOGW(TAG, "HEAP: int free=%uKB (min=%uKB)  dma free=%uB  psram free=%uKB  sock=%d  ring@%p[%s]",
                     (unsigned)(i_free / 1024), (unsigned)(i_min / 1024),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned)(p_free / 1024), sock_probe_free_cached(6, 10000),
                     (void *)s_ring_storage,
                     (((uintptr_t)s_ring_storage >> 24) == 0x4F) ? "INTERNAL" : "PSRAM");
        }
    }
}

// Stats (touched from RX context, snapshot from task)
static volatile uint32_t s_samples_this_period = 0;
static volatile int16_t  s_peak_left  = 0;
static volatile int16_t  s_peak_right = 0;
static volatile uint32_t s_dropped_this_period = 0;
static volatile uint32_t s_dropped_total = 0;   // running since boot (per-slot delta diag)
static int64_t s_period_start_us = 0;

// Discovered at runtime
static uint32_t s_sample_freq = 0;
static uint8_t  s_channels    = 0;
static uint8_t  s_bit_res     = 0;

// Set when the UAC stream (re)starts; cleared on the first batch of real
// samples, at which point the flat-spectrum floor is re-seeded so a stale
// floor from before a QMX power cycle doesn't linger.
static volatile bool s_flat_reset_pending = false;

// Consumer (stub DSP) stats
// Forward decls
static void audio_task(void *arg);
static void uac_lib_event_cb(uint8_t addr, uint8_t iface_num,
                             const uac_host_driver_event_t event, void *arg);
static void uac_dev_event_cb(uac_host_device_handle_t dev_hdl,
                             const uac_host_device_event_t event, void *arg);

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG, "Audio init (Phase 3.3 - ring-buffered int16 stereo + stub consumer)");

    s_evt_queue = xQueueCreate(EVT_QUEUE_LEN, sizeof(audio_evt_t));
    if (!s_evt_queue) return ESP_ERR_NO_MEM;

    // Allocate the 64 KB sample ring from PSRAM, not the scarce internal DRAM
    // that SDIO/USB host DMA depend on. This ring is CPU-accessed only (the
    // UAC background task copies samples in via xRingbufferSend, the FFT task
    // reads them out) — nothing DMAs into it directly — so a PSRAM backing is
    // safe and PSRAM bandwidth (~hundreds of MB/s) dwarfs the ~190 KB/s rate.
    // Frees ~64 KB internal, giving the WiFi-over-SDIO TX path the contiguous
    // DMA headroom it was starving for (see transport_drv_sta_tx crash).
    s_ring = xRingbufferCreateWithCaps(SAMPLE_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring) {
        // Record WHERE the ring storage landed so the heap watchdog can report
        // it: PSRAM on the P4 maps to 0x48000000+, internal SRAM to 0x4FF00000+.
        StaticRingbuffer_t *st = NULL;
        xRingbufferGetStaticBuffer(s_ring, &s_ring_storage, &st);
    }
    if (!s_ring) {
        ESP_LOGE(TAG, "Failed to create sample ring buffer (%d bytes)",
                 SAMPLE_RING_BYTES);
        return ESP_ERR_NO_MEM;
    }

    // IQ balance initialized from main.c after settings load
    iq_balance_set_enabled(true);  // Phase A: hardcoded ON for first test
    ESP_LOGI(TAG, "IQ balance correction: ENABLED");
    ESP_LOGI(TAG, "Sample ring buffer: %d bytes (~%lu ms @ 48k stereo int16)",
             SAMPLE_RING_BYTES,
             (unsigned long)(SAMPLE_RING_BYTES / 4 * 1000 / 48000));

    // Start the periodic internal-heap watchdog (every 10 s; v2 = O(1)-only,
    // see heap_watchdog_task's comment).
    psram_task_create(heap_watchdog_task, "heap_wd", 3072, NULL, 1, tskNO_AFFINITY);

    const uac_host_driver_config_t cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_lib_event_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = uac_host_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uac_host_install failed: 0x%x (%s)",
                 err, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "UAC host driver installed");

    // Priority 6 (#51 root-cause fix, was 3): audio_task is the only pump from
    // the UAC driver's tiny internal buffer (19200 B = 66 ms of audio) into the
    // sample ring. At priority 3 it sat BELOW taskLVGL (4, same core 0), and the
    // post-decode FT8 decode-list rebuild ran hundreds of ms of solid pri-4 LVGL
    // work - starving audio_task past the 66 ms elasticity, so the UAC driver
    // silently discarded ~200-650 ms of audio EVERY slot (measured sample-exact:
    // consecutive capture windows advanced ~174-177.5k samples instead of
    // 180000). That hole destroyed each signal's opening Costas sync array ->
    // sync scores collapsed 5-13 pts (SNR unchanged) -> decode yield fell from
    // ~60/slot (slot 0/1, before any decode/UI storm exists) to a fraction.
    // Nothing on this device outranks irreplaceable RF samples; the drain loop
    // is a trivial memcpy, so pri 6 costs the UI nothing measurable.
    BaseType_t ok = xTaskCreatePinnedToCore(
        audio_task, "audio_task", 4096, NULL, 6, &s_audio_task, 0);
    if (ok != pdPASS) return ESP_FAIL;

    return ESP_OK;
}

/* ⭐ WHICH DIAL EACH SAMPLE BELONGS TO (#298).
 *
 * A spectrum is built from audio that left the radio a while ago, so drawing it
 * against the dial as it is NOW puts it in the wrong place - the trace slides
 * the way the VFO went and every waterfall row carries a different error, which
 * is the diagonal smear seen on the bench.
 *
 * ⛔ AND A TIME-BASED CONSTANT CANNOT FIX IT. Measured twice on 2026-08-31 by
 * commanding a 2 kHz step and watching the raw bins arrive: the audio moved
 * between 565 and 653 ms on one run and between 104 and 312 ms on the next -
 * the same rig, minutes apart, better than a factor of two. Frame spacing was
 * ~100 ms, so that is not measurement noise. Any fixed SPECTRUM_LATENCY would
 * be right for one run and wrong for the next.
 *
 * So the dial travels WITH THE SAMPLES instead, indexed by a monotonic pair
 * counter rather than by wall time. Scheduling jitter, ring depth and how often
 * the FFT gets to run all stop mattering: sample number N was captured under
 * exactly one dial, whenever it happens to be processed.
 *
 * ⚠ ONE FIXED OFFSET REMAINS, and it is the one that is genuinely fixed:
 * AUDIO_USB_QUEUE_PAIRS. Samples reach this ring having already sat in the USB
 * isochronous queue, which is CONFIGURED depth - 8 URBs x 40 packets x 1 ms
 * (CONFIG_UAC_NUM_ISOC_URBS / _NUM_PACKETS_PER_URB, deliberately deep for #51).
 * That part cannot vary the way scheduling does. If those Kconfig values ever
 * change, this must change with them. */
#define AUDIO_USB_QUEUE_PAIRS   (320 * 48)      /* 320 ms at 48 kHz */
#define AUDIO_DIAL_TRAIL_N      24

static uint64_t s_pairs_written = 0;            /* monotonic, written side */
static uint64_t s_pairs_read    = 0;            /* monotonic, read side    */
static struct { uint64_t at_pair; uint32_t hz; } s_dial_trail[AUDIO_DIAL_TRAIL_N];
static int      s_dial_trail_n  = 0;
static uint32_t s_dial_now      = 0;

void audio_note_dial_hz(uint32_t hz)
{
    if (hz == s_dial_now) return;
    s_dial_now = hz;
    /* This dial applies to samples that reach the ring AFTER the ones already
     * queued in USB - hence the offset. Everything downstream is exact. */
    int i = s_dial_trail_n % AUDIO_DIAL_TRAIL_N;
    s_dial_trail[i].at_pair = s_pairs_written + AUDIO_USB_QUEUE_PAIRS;
    s_dial_trail[i].hz      = hz;
    s_dial_trail_n++;
}

/* The dial in force for the sample at monotonic index `pair`. */
static uint32_t dial_for_pair(uint64_t pair)
{
    uint32_t best = 0; uint64_t best_at = 0; bool found = false;
    int have = (s_dial_trail_n < AUDIO_DIAL_TRAIL_N) ? s_dial_trail_n : AUDIO_DIAL_TRAIL_N;
    for (int k = 0; k < have; k++) {
        int idx = (s_dial_trail_n - 1 - k) % AUDIO_DIAL_TRAIL_N;
        if (idx < 0) idx += AUDIO_DIAL_TRAIL_N;
        uint64_t at = s_dial_trail[idx].at_pair;
        if (at <= pair && (!found || at > best_at)) {
            best = s_dial_trail[idx].hz; best_at = at; found = true;
        }
    }
    return found ? best : s_dial_now;
}

uint32_t audio_dial_for_last_read(void)
{
    return dial_for_pair(s_pairs_read);
}

size_t audio_read_samples(int16_t *dst, size_t max_pairs, uint32_t timeout_ms)
{
    if (!s_ring || !dst || max_pairs == 0) return 0;

    size_t want_bytes = max_pairs * sizeof(int16_t) * 2;
    size_t got_bytes = 0;
    void *item = xRingbufferReceiveUpTo(
        s_ring, &got_bytes, pdMS_TO_TICKS(timeout_ms), want_bytes);
    if (!item) return 0;

    memcpy(dst, item, got_bytes);
    vRingbufferReturnItem(s_ring, item);
    size_t pairs = got_bytes / (sizeof(int16_t) * 2);
    s_pairs_read += pairs;
    return pairs;
}

size_t audio_ring_backlog_pairs(void)
{
    if (!s_ring) return 0;
    size_t free_bytes = xRingbufferGetCurFreeSize(s_ring);
    size_t used = (free_bytes < SAMPLE_RING_BYTES) ? (SAMPLE_RING_BYTES - free_bytes) : 0;
    return used / (sizeof(int16_t) * 2);
}

uint32_t audio_get_dropped_total(void)
{
    return s_dropped_total;
}

bool audio_uac_active(void)
{
    return s_uac_dev != NULL;
}

void audio_request_reset(void)
{
    s_flat_reset_pending = true;
}

static void uac_lib_event_cb(uint8_t addr, uint8_t iface_num,
                             const uac_host_driver_event_t event, void *arg)
{
    audio_evt_t e = { .addr = addr, .iface_num = iface_num };
    switch (event) {
    case UAC_HOST_DRIVER_EVENT_RX_CONNECTED:
        e.kind = AE_RX_CONNECTED;
        ESP_LOGI(TAG, "Lib event: RX_CONNECTED addr=%u iface=%u", addr, iface_num);
        if (s_evt_queue) xQueueSend(s_evt_queue, &e, 0);
        break;
    case UAC_HOST_DRIVER_EVENT_TX_CONNECTED:
        ESP_LOGI(TAG, "Lib event: TX_CONNECTED addr=%u iface=%u (ignored)",
                 addr, iface_num);
        break;
    default:
        ESP_LOGI(TAG, "Lib event %d addr=%u iface=%u", (int)event, addr, iface_num);
        break;
    }
}

static void uac_dev_event_cb(uac_host_device_handle_t dev_hdl,
                             const uac_host_device_event_t event, void *arg)
{
    audio_evt_t e = { .addr = 0, .iface_num = 0 };
    switch (event) {
    case UAC_HOST_DEVICE_EVENT_RX_DONE:
        e.kind = AE_RX_DONE;
        break;
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        e.kind = AE_TRANSFER_ERROR;
        break;
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        e.kind = AE_DISCONNECTED;
        break;
    default:
        return;
    }
    if (s_evt_queue) xQueueSend(s_evt_queue, &e, 0);
}

static void log_stats(void)
{
    int64_t now = esp_timer_get_time();
    if (s_period_start_us == 0) { s_period_start_us = now; return; }
    int64_t elapsed_us = now - s_period_start_us;
    if (elapsed_us < STATS_PERIOD_MS * 1000) return;

    uint32_t samples = s_samples_this_period;
    int16_t  pL = s_peak_left;
    int16_t  pR = s_peak_right;
    uint32_t dropped = s_dropped_this_period;
    s_samples_this_period = 0;
    s_peak_left = 0;
    s_peak_right = 0;
    s_dropped_this_period = 0;
    s_period_start_us = now;

    uint32_t pairs_per_sec = (uint32_t)((uint64_t)samples * 1000000ULL / (uint64_t)elapsed_us);

    // Dead-stream watchdog (Roy KI0ER, two field reports 2026-08-07: decode
    // list blank while TX still works; his reproducible trigger is exiting the
    // QMX's own menu, which stops the IQ audio stream and it never resumes).
    // The decode-side watchdog cannot catch this - it stands down during TX/QSO
    // (Roy was auto-answering) - but the SIGNATURE here is unambiguous: the UAC
    // device is open, CAT answers polls, and not one audio pair arrives. A
    // working QMX in IQ mode streams ~48000 pairs/s without exception.
    //
    // Escalation, deliberately slow and capped, cheapest step first:
    //   30 s  -> re-assert IQ mode (Q9 1;). Free and invisible. Added after
    //            the menu-visit trigger was understood: Q9 is SESSION state on
    //            the QMX, and a trip through its menu can drop it - in which
    //            case the radio is working perfectly and simply is not being
    //            asked for IQ audio any more. Nothing to reset, nothing to
    //            power-cycle; just ask again.
    //   60 s  -> soft reset of the audio pipeline (the same reset the decode
    //            watchdog uses).
    //   120 s -> ONE usb_replug() (VBUS cycle - the recovery Roy performs by
    //            hand as a QMX power-cycle, minus the walk). Cap of 2 replugs
    //            per connection so a genuinely dead radio cannot put the port
    //            in a cycle loop; any real audio resets everything.
    //
    // Stands down entirely while the operator has paused CAT: a deliberate trip
    // into the QMX's own menu produces this exact signature, and yanking VBUS
    // under the operator's hands would be the worst possible response to it.
    {
        static int s_silent_secs = 0;
        static int s_replug_used = 0;
        if (pairs_per_sec > 0 || !s_uac_dev || !cat_is_ready() || cat_user_pause_active()) {
            if (pairs_per_sec > 0) s_replug_used = 0;
            s_silent_secs = 0;
        } else {
            s_silent_secs += STATS_PERIOD_MS / 1000;
            if (s_silent_secs == 30) {
                ESP_LOGW(TAG, "dead stream: 30 s of silence with CAT alive - re-asserting IQ mode");
                cat_request_iq_reassert();
            } else if (s_silent_secs == 60) {
                ESP_LOGW(TAG, "dead stream: 60 s of silence with CAT alive - soft audio reset");
                audio_request_reset();
            } else if (s_silent_secs >= 120 && s_replug_used < 2) {
                s_replug_used++;
                s_silent_secs = 0;   // give the replug its own 120 s to prove itself
                ESP_LOGW(TAG, "dead stream: still silent - USB replug (attempt %d/2)", s_replug_used);
                usb_replug(2000);
            }
        }
    }

    // A partial 6-byte frame ever reaching us is the I/Q-interleave hazard, so
    // it is reported the moment it is non-zero rather than only under DROPPED.
    // Cumulative, not per-second: one event is the whole story.
    if (s_misalign_events > 0) {
        ESP_LOGW(TAG, "RX %u pairs/s peak L=%d R=%d  FRAME-MISALIGN=%u ev / %u B (carried, not dropped)",
                 (unsigned)pairs_per_sec, (int)pL, (int)pR,
                 (unsigned)s_misalign_events, (unsigned)s_misalign_bytes);
    } else if (dropped > 0) {
        ESP_LOGW(TAG, "RX %u pairs/s peak L=%d R=%d DROPPED=%u (ring full)",
                 (unsigned)pairs_per_sec, (int)pL, (int)pR, (unsigned)dropped);
    } else {
        // INFO (#51 instrumentation, was DEBUG): the USB-side delivery rate is
        // the stage-2 loss probe - expect ~48000 pairs/s; a deficit here with
        // drop=0 means the UAC driver discarded transfers upstream.
        //
        // ⭐ Every 10 s when healthy, not every second (2026-09-13, operator:
        // "we need to be in control of what is happening here"): 60 identical
        // lines a minute buried everything else in the capture. An abnormal
        // second - a rate more than 2 % off 48 k, or a stream that starts or
        // stops - still logs at once, so the #51 loss probe keeps its resolution
        // exactly where it matters.
        //
        // ⚠ The band is TWO USB transfers wide, not 2 %. One transfer is 40
        // packets = 1920 pairs, and a 1 s window catches 24, 25 or 26 of them
        // depending on where its edges fall, so a perfect stream reads
        // 46080/48000/49920 (measured 46859..49507 on a healthy bench). A 2 %
        // band flagged that jitter as abnormal about once a second. Real loss
        // shows as the 10 s mean, which is printed on the quiet line.
        static int      s_quiet = 0;
        static uint32_t s_prev_pps = 0;
        static uint32_t s_sum_pps = 0;
        bool odd = (pairs_per_sec > 0 && (pairs_per_sec < 46000 || pairs_per_sec > 50000)) ||
                   ((pairs_per_sec == 0) != (s_prev_pps == 0));
        s_prev_pps = pairs_per_sec;
        s_sum_pps += pairs_per_sec;
        s_quiet++;
        if (odd || s_quiet >= 10) {
            ESP_LOGI(TAG, "RX %u pairs/s peak L=%d R=%d  (mean %u over %d s)",
                     (unsigned)pairs_per_sec, (int)pL, (int)pR,
                     (unsigned)(s_sum_pps / (uint32_t)s_quiet), s_quiet);
            s_quiet = 0;
            s_sum_pps = 0;
        }
    }
}

// Decode a packed 24-bit little-endian signed PCM sample (3 bytes) into int32_t.
static inline int32_t s24_to_s32(const uint8_t *p)
{
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if (u & 0x00800000U) u |= 0xFF000000U;
    return (int32_t)u;
}

// Returns true if this poll actually moved audio. The caller uses that to decide
// whether to yield: see audio_task().
// Bytes left over from the previous read when it did not end on a whole stereo
// frame, carried into the next one. A frame is 3 bytes I + 3 bytes Q on 24-bit
// streams (the QMX) or 2 bytes I + 2 bytes Q on 16-bit streams (a line-in
// dongle); the byte budget below covers the largest, 6.
//
// WHY THIS EXISTS. The stream is 3 bytes I + 3 bytes Q, and NOTHING in the path
// re-synchronises it. `pairs = bytes_read / 6` used to discard the remainder, so
// a single read that ended mid-frame shifted every following sample by 1-5
// bytes for the REST OF THE UAC SESSION: a 3-byte shift swaps I and Q outright,
// the others build both channels from mismatched bytes. Either way the image
// rejection collapses and mirror signals appear.
//
// That failure survives a QMX power cycle (the UAC device stays open - see the
// v0.15.5 note) and is cleared only by a Tab5 reboot, which is exactly what
// Roy KI0ER reported on 2026-08-14: restarting the radio changed nothing,
// restarting the Tab5 fixed it.
//
// Two ways a partial frame can reach us, neither guarded: `_ring_buffer_pop()`
// returns whatever it has when its timeout expires with no frame alignment, and
// the driver pushes `actual_num_bytes` per ISO packet, which its own comment
// says "may be less than requested" - this file's own header already records
// truncated UAC chunks as real on this radio.
//
// s_misalign_events is the measurement. It is a modulo on a hot path and one
// counter, nothing walked (cyan-flash rule), and it reports on the existing
// 1 Hz line rather than adding one. If it stays 0 in the field then partial
// frames are not happening and the mirror images have another cause.
static bool process_rx(void)
{
    // Raw packed bytes from the I/Q UAC source (24-bit = 3B I + 3B Q per pair,
    // 16-bit = 2B I + 2B Q per pair).
    static uint8_t raw[RX_BUF_BYTES];
    // Decoded int16 stereo pairs, sized for the SMALLEST frame (4 B/pair) so
    // the worst-case pair count fits. 24-bit fills proportionally fewer.
    static int16_t decoded[(RX_BUF_BYTES / 4) * 2 + 2];

    // Whether this call moved any audio at all. NOT simply "the last read
    // returned data": the drain loop always ends on an empty read, so a healthy
    // stream would otherwise look idle every single poll.
    bool got_data = false;

    // Phase 5.7: drain loop. First read waits up to 25 ms for data;
    // subsequent reads in the same call are non-blocking so we drain the
    // UAC driver buffer in one polling iteration of audio_task.
    bool first_read = true;
    while (1) {
        uint32_t bytes_read = 0;
        uint32_t to = first_read ? pdMS_TO_TICKS(25) : 0;
        first_read = false;
        // Put any partial frame from last time back at the front, so the frame
        // it belongs to is completed rather than lost.
        if (s_carry_len) memcpy(raw, s_carry, s_carry_len);

        esp_err_t err = uac_host_device_read(s_uac_dev, raw + s_carry_len,
                                             sizeof(raw) - s_carry_len,
                                             &bytes_read, to);
        if (err != ESP_OK || bytes_read == 0) {
            // No data this poll: the QMX may be mid power-cycle. Mark the
            // flat-spectrum floor for re-seeding once real samples resume.
            s_flat_reset_pending = true;
            return got_data;
        }

        // Each stereo pair is 6 bytes (3B I + 3B Q, little-endian signed
        // 24-bit) on the QMX, or 4 bytes (2B I + 2B Q, little-endian signed
        // 16-bit) on a typical line-in dongle. Frame size comes from the
        // bit depth this stream negotiated, so the pair count and carry are
        // correct for whichever device is feeding us.
        size_t bpp = (s_bit_res > 16) ? 6 : 4;
        size_t avail = s_carry_len + bytes_read;
        size_t pairs = avail / bpp;
        size_t rem   = avail - pairs * bpp;

        // Keep the partial frame for the next read instead of dropping it.
        // Dropping is what shifted the I/Q interleave permanently.
        if (rem) {
            s_misalign_events++;
            s_misalign_bytes += (uint32_t)rem;
            memcpy(s_carry, raw + pairs * bpp, rem);
        }
        s_carry_len = rem;

        if (pairs == 0) {
            s_flat_reset_pending = true;
            return got_data;
        }
        got_data = true;

        if (s_flat_reset_pending) {
            s_flat_reset_pending = false;
            ui_flat_mode_reset();
        }

        int16_t local_peak_L = s_peak_left;
        int16_t local_peak_R = s_peak_right;
        bool swap_iq = radio_iq_swap();

        for (size_t i = 0; i < pairs; i++) {
            const uint8_t *p = raw + bpp * i;
            int32_t L, R;
            if (bpp == 6) {
                L = s24_to_s32(p) >> 8;
                R = s24_to_s32(p + 3) >> 8;
            } else {
                L = (int16_t)(p[0] | (p[1] << 8));
                R = (int16_t)(p[2] | (p[3] << 8));
            }
            if (L > 32767) L = 32767; else if (L < -32768) L = -32768;
            if (R > 32767) R = 32767; else if (R < -32768) R = -32768;

            int16_t Is = (int16_t)L;
            int16_t Qs = (int16_t)R;

            // Peaks from the raw decode, BEFORE balance/swap correction - same
            // as the historical behaviour; the correction is a sample-tweak,
            // not something the level meters should track.
            int16_t aL = (L < 0) ? (int16_t)-L : (int16_t)L;
            int16_t aR = (R < 0) ? (int16_t)-R : (int16_t)R;
            if (aL > local_peak_L) local_peak_L = aL;
            if (aR > local_peak_R) local_peak_R = aR;

            if (swap_iq) {
                int16_t t = Is; Is = Qs; Qs = t;
            }
            iq_balance_apply(&Is, &Qs);
            decoded[2*i]     = Is;
            decoded[2*i + 1] = Qs;
        }

        s_peak_left = local_peak_L;
        s_peak_right = local_peak_R;
        s_samples_this_period += pairs;

        size_t bytes_to_push = pairs * sizeof(int16_t) * 2;
        BaseType_t sent = xRingbufferSend(s_ring, decoded, bytes_to_push, 0);
        if (sent == pdTRUE) s_pairs_written += pairs;
        if (sent != pdTRUE) {
            s_dropped_this_period += pairs;
            s_dropped_total += pairs;
        }

        // Feeds rx_audio.c's demodulator (any supported mode, not just CW - see
        // rx_audio.h). Bool-gated no-op when RX audio is off; measured clean
        // over an 8.7 h live FT8 session (zero ring drops/skips) before this
        // call was trusted again - the v0.18.5 band-aid's "this costs 2-3x FT8
        // decode" claim did not reproduce once isolated from the actual root
        // cause (a priority-6 consumer task preempting fft_task - see
        // rx_audio.c). See memory project_rx_audio_track.md for the A/B.
        dsp_rxaudio_forward(decoded, pairs);
        // Loop back for another non-blocking drain read.
    }
}

static esp_err_t open_and_start(uint8_t addr, uint8_t iface_num)
{
    /* The panadapter's audio is the configured I/Q UAC source. On the QMX that
     * is the radio's own device; with a KX3 the I/Q comes from a separate
     * line-in dongle AND the KX3's own USB audio (demodulated AF) also shows up
     * as a UAC on the same bus - which must be left alone. Everything below
     * follows this VID/PID, so only the source opens. */
    uint16_t iq_vid, iq_pid;
    radio_iq_vid_pid(&iq_vid, &iq_pid);
    if ((iq_vid | iq_pid) == 0) {
        ESP_LOGE(TAG, "I/Q source not configured (radio=%s, VID/PID 0:0) - "
                      "panadapter stays silent. On a KX3 set the dongle's VID:PID "
                      "in the settings drawer (I/Q dongle).",
                 radio_kind_name());
        return ESP_ERR_NOT_FOUND;
    }

    const uac_host_device_config_t dev_cfg = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = INTERNAL_RX_BUF_BYTES,
        .buffer_threshold = INTERNAL_RX_BUF_BYTES / 4,
        .callback = uac_dev_event_cb,
        .callback_arg = NULL,
    };
    esp_err_t err = uac_host_device_open(&dev_cfg, &s_uac_dev);
    if (err != ESP_OK) {
        /* The connect event named a device we could not open - most likely the
         * OTHER UAC on the bus (a KX3's AF audio), or a device mid
         * enumeration. Fall back to the configured I/Q source by VID/PID so a
         * race between the two UAC connects cannot strand the panadapter. */
        ESP_LOGW(TAG, "device_open(addr=%u iface=%u) failed: 0x%x (%s) - "
                      "trying the configured I/Q source by VID/PID",
                 addr, iface_num, err, esp_err_to_name(err));
        err = uac_host_device_open_with_vid_pid(iq_vid, iq_pid, &dev_cfg, &s_uac_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I/Q source %04X:%04X not openable: 0x%x (%s)",
                     iq_vid, iq_pid, err, esp_err_to_name(err));
            return err;
        }
    }

    /* Identify what we actually have open. Two cases: the event device IS the
     * configured source (QMX, or a dongle whose event won the race), or it is
     * a different UAC and the id-based reopen above just swapped it. In both
     * cases the READ shows the truth; if it is still not the source, drop it
     * and take the id-based path for real. This line is also the bus dump that
     * names a new dongle/KX3 to type into the drawer's "I/Q dongle" picker. */
    {
        uac_host_dev_info_t info = {0};
        if (uac_host_get_device_info(s_uac_dev, &info) == ESP_OK) {
            if (info.VID != iq_vid || info.PID != iq_pid) {
                ESP_LOGW(TAG, "UAC at addr=%u is %04X:%04X (%ls), NOT the I/Q "
                              "source %04X:%04X - closing and reopening the right device",
                         addr, (unsigned)info.VID, (unsigned)info.PID,
                         info.iProduct, (unsigned)iq_vid, (unsigned)iq_pid);
                uac_host_device_close(s_uac_dev);
                s_uac_dev = NULL;
                err = uac_host_device_open_with_vid_pid(iq_vid, iq_pid, &dev_cfg, &s_uac_dev);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "I/Q source %04X:%04X not openable: 0x%x (%s)",
                             iq_vid, iq_pid, err, esp_err_to_name(err));
                    return err;
                }
            } else {
                ESP_LOGI(TAG, "I/Q source open: VID=%04X PID=%04X addr=%u iface=%u",
                         (unsigned)info.VID, (unsigned)info.PID, addr, iface_num);
            }
        } else {
            ESP_LOGI(TAG, "Device opened (addr=%u iface=%u)", addr, iface_num);
        }
    }

    uac_host_dev_alt_param_t alt = {0};
    err = uac_host_get_device_alt_param(s_uac_dev, 1, &alt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_device_alt_param failed: 0x%x", err);
        uac_host_device_close(s_uac_dev);
        s_uac_dev = NULL;
        return err;
    }
    /* Log the WHOLE rate set, not just the first entry. The first entry is not
     * the rate we are about to use, and on a continuous-range device it is not
     * even a rate (it is the bottom of the range) - so a one-line "first_rate"
     * log sent whoever is debugging this to the wrong conclusion. */
    ESP_LOGI(TAG, "Alt 1: channels=%u, %u-bit, %u discrete rate(s)",
             alt.channels, alt.bit_resolution, alt.sample_freq_type);
    if (alt.sample_freq_type > 0) {
        uint32_t n = alt.sample_freq_type;
        if (n > UAC_FREQ_NUM_MAX) n = UAC_FREQ_NUM_MAX;
        for (uint32_t i = 0; i < n; i++)
            ESP_LOGI(TAG, "    offers %lu Hz",
                     (unsigned long)alt.sample_freq[i]);
    } else {
        ESP_LOGI(TAG, "    offers a continuous %lu-%lu Hz range",
                 (unsigned long)alt.sample_freq_lower,
                 (unsigned long)alt.sample_freq_upper);
    }

    /* The DSP pipeline is fixed at 48 kHz int16 stereo, with no resampler
     * anywhere. Anything it cannot be fed directly is refused rather than
     * tolerated: a spectrum drawn at the wrong rate is an instrument that
     * looks right and reads wrong. Bit depth and sample rate are both hard
     * errors below. Channel count stays a warning only - a mono source still
     * produces a usable (if half-width) display, and refusing it would turn a
     * degraded picture into no picture at all. */
    if (alt.channels != 2) {
        ESP_LOGW(TAG, "I/Q source is %u-channel - this firmware de-interleaves "
                      "I/Q from a STEREO pair, so a mono source will not decode",
                 alt.channels);
    }
    if (alt.bit_resolution != 16 && alt.bit_resolution != 24) {
        ESP_LOGE(TAG, "Unsupported I/Q bit depth %u-bit (16 and 24 only)", alt.bit_resolution);
        uac_host_device_close(s_uac_dev);
        s_uac_dev = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* ⛔ SELECT 48 kHz EXPLICITLY. NEVER `alt.sample_freq[0]`.
     *
     * DSP_SAMPLE_RATE_HZ (main/dsp/dsp.h) is 48000 and there is NO resampler
     * anywhere in the chain - the FFT bin width is derived from it directly
     * (48000/1024 = 46.875 Hz). So whatever rate we open at IS the rate the
     * spectrum is drawn at. Taking the first rate the device happens to list
     * and merely warning about it was quietly wrong twice over:
     *
     *   1. Devices list rates in whatever order their descriptors put them. A
     *      device offering both 44.1k and 48k that lists 44.1k first was driven
     *      at 44.1k: every signal 8.8% off (48000/44100), ~264 Hz out at 3 kHz
     *      into a 14 MHz band, and FT8 tones landing in the wrong bins, so it
     *      never decoded - while the UI looked perfectly healthy.
     *      The DigiRig Mobile's CM108 is exactly this device: its datasheet
     *      offers two rates and nothing else, "48K / 44.1KHz Sampling Rate for
     *      Both Playback and Recording", so the order of a two-entry list
     *      decided correctness.
     *   2. When sample_freq_type == 0 the field is a CONTINUOUS range, and
     *      sample_freq[0] is sample_freq_lower by union aliasing. The old code
     *      therefore requested the bottom of the range and called it a rate.
     *
     * uac_host_device_start() also matches channels+bit_resolution+rate against
     * an alt setting exactly, so asking for a rate the device does not offer
     * fails there anyway - better to refuse here, by name, than there.
     *
     * A refused 48 kHz is now a hard error like the bit-depth check above,
     * not a warning: a wrong-pitched spectrum is an instrument that lies,
     * which is worse than one that declares itself unusable. */
    const uint32_t want_hz = DSP_SAMPLE_RATE_HZ;
    bool rate_ok = false;
    if (alt.sample_freq_type > 0) {
        /* sample_freq_type is the COUNT of discrete rates, and the driver
         * truncates a longer list to UAC_FREQ_NUM_MAX (default 4) when it
         * parses tSamFreq - so clamp to the same bound rather than trusting
         * the count blindly. */
        uint32_t n = alt.sample_freq_type;
        if (n > UAC_FREQ_NUM_MAX) n = UAC_FREQ_NUM_MAX;
        for (uint32_t i = 0; i < n; i++)
            if (alt.sample_freq[i] == want_hz) { rate_ok = true; break; }
    } else {
        rate_ok = (alt.sample_freq_lower <= want_hz && alt.sample_freq_upper >= want_hz);
    }

    if (!rate_ok) {
        if (alt.sample_freq_type > 0) {
            uint32_t n = alt.sample_freq_type;
            if (n > UAC_FREQ_NUM_MAX) n = UAC_FREQ_NUM_MAX;
            ESP_LOGE(TAG, "I/Q source offers no %lu Hz rate (the DSP is pinned "
                          "to 48 kHz and cannot resample). It offers %lu:",
                     (unsigned long)want_hz, (unsigned long)n);
            for (uint32_t i = 0; i < n; i++)
                ESP_LOGE(TAG, "    %lu Hz",
                         (unsigned long)alt.sample_freq[i]);
        } else {
            ESP_LOGE(TAG, "I/Q source offers a %lu-%lu Hz range, which excludes "
                          "the 48 kHz the DSP is pinned to",
                     (unsigned long)alt.sample_freq_lower,
                     (unsigned long)alt.sample_freq_upper);
        }
        uac_host_device_close(s_uac_dev);
        s_uac_dev = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uac_host_stream_config_t stream_cfg = {
        .channels = alt.channels,
        .bit_resolution = alt.bit_resolution,
        .sample_freq = want_hz,
        .flags = 0,
    };
    err = uac_host_device_start(s_uac_dev, &stream_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_start failed: 0x%x (%s)", err, esp_err_to_name(err));
        uac_host_device_close(s_uac_dev);
        s_uac_dev = NULL;
        return err;
    }

    s_channels    = alt.channels;
    s_bit_res     = alt.bit_resolution;
    s_sample_freq = want_hz;
    s_period_start_us = esp_timer_get_time();
    s_samples_this_period = 0;
    s_peak_left = 0;
    s_peak_right = 0;
    s_dropped_this_period = 0;
    s_flat_reset_pending = true;

    ESP_LOGI(TAG, "UAC stream started: %lu Hz, %u ch, %u-bit",
             (unsigned long)s_sample_freq, s_channels, s_bit_res);
    return ESP_OK;
}

static void audio_task(void *arg)
{
    // Phase 5.7: polling loop mirroring qrp_companion (Zhenxing Han, N6HAN) mic_task pattern.
    // audio_task pulls UAC bytes in a tight loop with a 200 ms read timeout while the device
    // is up, and yields once per iteration for the watchdog. RX_DONE / TRANSFER_ERROR events
    // are no longer consumed; only connect / disconnect events are handled out-of-band.
    while (1) {
        // Connect / disconnect events still arrive on the queue (non-blocking peek).
        audio_evt_t e;
        if (xQueueReceive(s_evt_queue, &e, 0) == pdTRUE) {
            switch (e.kind) {
            case AE_RX_CONNECTED:
                if (s_uac_dev == NULL) {
                    // A new UAC session starts on a frame boundary; anything
                    // held from the old one belongs to a stream that is gone.
                    audio_reset_frame_alignment();
                    open_and_start(e.addr, e.iface_num);
                } else {
                    ESP_LOGW(TAG, "Already streaming; ignoring extra RX_CONNECTED");
                }
                break;
            case AE_DISCONNECTED:
                ESP_LOGW(TAG, "UAC disconnected, cleaning up");
                audio_reset_frame_alignment();
                if (s_uac_dev) {
                    uac_host_device_stop(s_uac_dev);
                    uac_host_device_close(s_uac_dev);
                    s_uac_dev = NULL;
                }
                break;
            default:
                break;
            }
        }
        if (s_uac_dev) {
            // No vTaskDelay when audio is flowing - 10 ms at the default tick
            // rate starved the read (and #51 is a standing reminder of what
            // starving this path costs).
            //
            // But a poll that moved NOTHING must yield, or a dead device pegs
            // this core. Hardware-observed 2026-08-05: a QMX power-cycle did not
            // deliver AE_DISCONNECTED (a documented quirk of this hardware - the
            // UAC handle just goes quiet), so s_uac_dev still pointed at the dead
            // device. On a merely SILENT device the read honours its 25 ms
            // timeout, so this loop self-limits to ~40 Hz and all is well; on an
            // INVALID-STATE device it fails INSTANTLY, and this loop span flat
            // out at priority 6. Result: core 0 at 0% idle, LVGL starved, the UI
            // frozen and no recovery possible - the USB host logged
            // "usbh_devs_open error: ESP_ERR_INVALID_STATE" every 50 ms
            // indefinitely while nothing could act on it.
            //
            // Costs nothing when streaming (got_data == true), and turns the
            // dead-device case from a locked-up radio into an idle one.
            if (!process_rx()) vTaskDelay(pdMS_TO_TICKS(5));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        log_stats();
    }
}
// Orderly UAC shutdown - see util/usb_shutdown.h.
//
// This is the half that matters most for the QMX. uac_host_device_stop() sets
// the streaming interface back to alternate setting 0, which is the device's
// signal to stop producing isochronous audio; uac_host_device_close() then
// releases it. A host that simply disappears does neither, leaving the radio's
// USB stack mid-stream on an endpoint nobody is servicing any more - which is
// the state the QMX appears not to recover from.
//
// Runs on the caller's task, not audio_task: by the time this is called the
// system is going down, and waiting for a task that polls USB (which is exactly
// what we are dismantling) would be circular. s_uac_dev is cleared FIRST so
// process_rx() cannot touch the handle we are closing.
void audio_usb_shutdown(void)
{
    uac_host_device_handle_t dev = s_uac_dev;
    if (!dev) {
        ESP_LOGI(TAG, "shutdown: no UAC device open");
        return;
    }
    s_uac_dev = NULL;              // audio_task's next poll sees no device
    vTaskDelay(pdMS_TO_TICKS(30)); // let an in-flight read return

    esp_err_t e1 = uac_host_device_stop(dev);
    esp_err_t e2 = uac_host_device_close(dev);
    ESP_LOGI(TAG, "shutdown: UAC stream stopped (%s) and closed (%s)",
             esp_err_to_name(e1), esp_err_to_name(e2));
}
