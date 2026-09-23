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
#define KEY_DONG_VID  "dkr_vid"  /* u32 I/Q line-in dongle */
#define KEY_DONG_PID  "dkr_pid"
#define KEY_IQ_SWAP   "iq_swap"  /* u8: flip I/Q channels */

static radio_kind_t s_kind = RADIO_QMX;
static uint16_t s_kx3_vid = 0, s_kx3_pid = 0;
static uint16_t s_dongle_vid = 0, s_dongle_pid = 0;
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
    if (nvs_get_u32(h, KEY_DONG_VID, &u32) == ESP_OK) s_dongle_vid = (uint16_t)u32;
    if (nvs_get_u32(h, KEY_DONG_PID, &u32) == ESP_OK) s_dongle_pid = (uint16_t)u32;
    if (nvs_get_u8(h, KEY_IQ_SWAP, &u8) == ESP_OK)    s_iq_swap   = (u8 != 0);
    nvs_close(h);
    s_loaded = true;

    ESP_LOGI(TAG, "profile: radio=%s  CAT=%04X:%04X  I/Q source=%04X:%04X  iq_swap=%u",
             radio_kind_name(),
             s_kind == RADIO_QMX ? QMX_VID : s_kx3_vid,
             s_kind == RADIO_QMX ? QMX_PID : s_kx3_pid,
             s_kind == RADIO_QMX ? QMX_VID : s_dongle_vid,
             s_kind == RADIO_QMX ? QMX_PID : s_dongle_pid,
             s_iq_swap ? 1 : 0);

    if (s_kind == RADIO_KX3 && ((s_kx3_vid | s_kx3_pid) == 0)) {
        ESP_LOGW(TAG, "KX3 CAT identity not set (kx3 VID/PID = 0) - CAT will refuse "
                      "to open. Set it via radio_set_kx3_vid_pid().");
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