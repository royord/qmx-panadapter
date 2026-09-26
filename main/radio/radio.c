#include "radio.h"

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "radio";

/* The QMX composite device (CAT CDC + I/Q UAC in one VID/PID).
 * ST disclaims VID 0x0483 here exactly as it does for cat.c's own copy, so a
 * future reader cannot "fix" one half without the other. */
#define QMX_VID  0x0483
#define QMX_PID  0xA34C

#define NVS_NAMESPACE "radio"
#define KEY_KIND      "kind"     /* u8: 0 = QMX (default), 1 = KX3 */
#define KEY_KX3_VID   "kx3_vid"  /* u32 KX3 CAT device */
#define KEY_KX3_PID   "kx3_pid"
#define KEY_CAT_IFACE "cat_if"  /* u8: which USB interface of it carries CAT */
#define KEY_DONG_VID  "dkr_vid"  /* u32 I/Q line-in dongle */
#define KEY_DONG_PID  "dkr_pid"
#define KEY_IQ_SWAP   "iq_swap"  /* u8: flip I/Q channels */
#define KEY_BAUD      "baud"     /* u32 KX3 CAT baud (QMX is fixed at 38400) */
#define CAT_BAUD_DEFAULT 38400

bool radio_cat_baud_valid(uint32_t baud);   /* forward decl: load_all() uses it */

static radio_kind_t s_kind = RADIO_QMX;
static uint16_t s_kx3_vid = 0, s_kx3_pid = 0;
static uint8_t  s_cat_iface = 0;
static uint16_t s_dongle_vid = 0, s_dongle_pid = 0;
static uint32_t s_baud = CAT_BAUD_DEFAULT;
static bool     s_iq_swap = false;
static bool     s_loaded = false;

static void load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;

    uint8_t u8 = 0; uint32_t u32 = 0;
    if (nvs_get_u8(h, KEY_KIND, &u8) == ESP_OK && u8 <= RADIO_KX3) s_kind = (radio_kind_t)u8;
    if (nvs_get_u32(h, KEY_KX3_VID, &u32) == ESP_OK)  s_kx3_vid   = (uint16_t)u32;
    if (nvs_get_u32(h, KEY_KX3_PID, &u32) == ESP_OK)  s_kx3_pid   = (uint16_t)u32;
    if (nvs_get_u8(h, KEY_CAT_IFACE, &u8) == ESP_OK)   s_cat_iface = (u8 < 4) ? u8 : 0;
    if (nvs_get_u32(h, KEY_DONG_VID, &u32) == ESP_OK) s_dongle_vid = (uint16_t)u32;
    if (nvs_get_u32(h, KEY_DONG_PID, &u32) == ESP_OK) s_dongle_pid = (uint16_t)u32;
    if (nvs_get_u32(h, KEY_BAUD, &u32) == ESP_OK)     s_baud      = radio_cat_baud_valid(u32) ? u32 : CAT_BAUD_DEFAULT;
    if (nvs_get_u8(h, KEY_IQ_SWAP, &u8) == ESP_OK)    s_iq_swap   = (u8 != 0);
    nvs_close(h);
    s_loaded = true;

    ESP_LOGI(TAG, "profile: radio=%s  CAT=%04X:%04X if%u  I/Q source=%04X:%04X  iq_swap=%u",
             radio_kind_name(),
             s_kind == RADIO_QMX ? QMX_VID : s_kx3_vid,
             s_kind == RADIO_QMX ? QMX_PID : s_kx3_pid,
             (unsigned)(s_kind == RADIO_QMX ? 0 : s_cat_iface),
             s_kind == RADIO_QMX ? QMX_VID : s_dongle_vid,
             s_kind == RADIO_QMX ? QMX_PID : s_dongle_pid,
             s_iq_swap ? 1 : 0);

    if (s_kind == RADIO_KX3 && ((s_kx3_vid | s_kx3_pid) == 0)) {
        ESP_LOGW(TAG, "KX3 CAT identity not set (kx3 VID/PID = 0) - will open by "
                      "protocol probe on first plug and learn it.");
    }
    if (s_kind == RADIO_KX3 && ((s_dongle_vid | s_dongle_pid) == 0)) {
        ESP_LOGW(TAG, "I/Q dongle identity not set (VID/PID = 0) - the panadapter "
                      "will stay silent. Set it via radio_set_dongle_vid_pid().");
    }
}

void radio_init(void)
{
    if (s_loaded) return;
    load_all();
}

radio_kind_t radio_get_kind(void) { return s_kind; }

const char *radio_kind_name(void)
{
    return s_kind == RADIO_KX3 ? "KX3" : "QMX";
}

void radio_cat_vid_pid(uint16_t *vid, uint16_t *pid)
{
    radio_init();
    if (s_kind == RADIO_QMX) { *vid = QMX_VID; *pid = QMX_PID; }
    else                     { *vid = s_kx3_vid; *pid = s_kx3_pid; }
}

uint8_t radio_cat_iface(void)
{
    radio_init();
    /* The QMX is a known composite: CAT is interface 0. Only a KX3 that the
     * probe found somewhere else has a stored index worth honouring. */
    return (s_kind == RADIO_QMX) ? 0 : s_cat_iface;
}

void radio_iq_vid_pid(uint16_t *vid, uint16_t *pid)
{
    radio_init();
    if (s_kind == RADIO_QMX) { *vid = QMX_VID; *pid = QMX_PID; }
    else                     { *vid = s_dongle_vid; *pid = s_dongle_pid; }
}

bool radio_iq_over_usb(void)
{
    return radio_get_kind() == RADIO_QMX;
}

bool radio_iq_swap(void) { return s_iq_swap; }
void radio_set_iq_swap(bool swap)
{
    s_iq_swap = swap;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, KEY_IQ_SWAP, swap ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void radio_set_kind(radio_kind_t kind)
{
    if (kind > RADIO_KX3) return;
    s_kind = kind;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, KEY_KIND, (uint8_t)kind);
    nvs_commit(h);
    nvs_close(h);
}

static void store_u32(const char *key, uint16_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static void store_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

void radio_set_kx3_vid_pid(uint16_t vid, uint16_t pid)
{
    s_kx3_vid = vid; s_kx3_pid = pid;
    store_u32(KEY_KX3_VID, vid);
    store_u32(KEY_KX3_PID, pid);
}

void radio_set_dongle_vid_pid(uint16_t vid, uint16_t pid)
{
    s_dongle_vid = vid; s_dongle_pid = pid;
    store_u32(KEY_DONG_VID, vid);
    store_u32(KEY_DONG_PID, pid);
}

bool radio_cat_baud_valid(uint32_t baud)
{
    return baud == 4800 || baud == 9600 || baud == 19200 || baud == 38400;
}

uint32_t radio_get_cat_baud(void)
{
    /* The QMX is fixed at 38400; only the KX3's stored baud is user-pickable. */
    if (radio_get_kind() == RADIO_QMX) return CAT_BAUD_DEFAULT;
    return s_baud;
}

void radio_set_cat_baud(uint32_t baud)
{
    if (!radio_cat_baud_valid(baud)) {
        ESP_LOGW(TAG, "ignoring baud %lu (must be one of %lu/%lu/%lu/%lu)",
                 (unsigned long)baud,
                 (unsigned long)4800, (unsigned long)9600,
                 (unsigned long)19200, (unsigned long)38400);
        return;
    }
    s_baud = baud;
    store_u32(KEY_BAUD, baud);
    ESP_LOGI(TAG, "KX3 CAT baud set to %lu", (unsigned long)baud);
}

void radio_note_cat_found(radio_kind_t kind, uint16_t vid, uint16_t pid, uint8_t iface)
{
    /* The caller classifies from the ANSWER, not from the VID/PID: a probe that
     * reached the radio by USB address alone (already plugged in before the
     * new-dev callback existed) has no VID/PID to compare, and "not the QMX"
     * would be a guess that pins the wrong radio forever. */
    if (kind > RADIO_KX3) return;
    radio_set_kind(kind);
    if (kind == RADIO_KX3) {
        /* 0/0 = not learned this time (address-only probe). Keep whatever pin a
         * previous boot managed, rather than overwriting a good one with 0:0. */
        if ((vid | pid) != 0) radio_set_kx3_vid_pid(vid, pid);
        /* The interface index is only meaningful next to a VID/PID pin - with
         * 0/0 the next boot probes again anyway and re-learns it. */
        if ((vid | pid) != 0 && iface < 4) {
            s_cat_iface = iface;
            store_u8(KEY_CAT_IFACE, iface);
        }
        ESP_LOGW(TAG, "⚡ CAT probe identified a KX3 (%04X:%04X if%u) - %s",
                 vid, pid, (unsigned)iface,
                 (vid | pid) ? "pinned for next boot"
                             : "identity unknown, will probe again next boot");
    } else {
        ESP_LOGW(TAG, "⚡ CAT probe identified the QMX (%04X:%04X)", vid, pid);
    }
}
