// KX3 I/Q dongle VID/PID modal.
//
// WHY THIS EXISTS. On a QMX the panadapter's I/Q arrives on the radio's own
// USB device, so radio_iq_vid_pid() returns the QMX's VID/PID and the audio
// path just works. A KX3 has NO digital I/Q output at all: its receiver hands
// analog I/Q out of the "RX I/Q" 2.5 mm jack, so the spectrum has to be fed by a
// line-in UAC dongle on the same host port (through a hub, alongside the KX3's
// own CAT CDC). For a KX3, therefore, radio_iq_vid_pid() returns the DONGLE's
// VID/PID - read from NVS keys "dkr_vid"/"dkr_pid".
//
// Those keys have a writer, radio_set_dongle_vid_pid(), and until this modal it
// had NO CALLER: no UI, no autodetect. So a KX3 booted with a dongle attached
// left them 0:0 and audio.c's open_and_start() refused outright - logging
// "Set it with radio_set_dongle_vid_pid()", an instruction to call a function
// the operator has no way to reach. This modal is that way in.
//
// The operator learns the numbers from the log: audio.c dumps every UAC on the
// bus that is not the configured I/Q source, by address, VID:PID and product
// string. That is also the only way to tell the line-in dongle from the KX3's
// own AF (demodulated audio) UAC, which is deliberately left closed.
//
// Structurally a copy of identity_config.c: two text fields, hex keyboard,
// Save/Cancel, physical-keyboard Enter/Esc. A shared N-field text modal is a
// future refactor (wifi_config.c and memory channels v2 want it too).

#include "kx3_iq_modal.h"
#include "ui_theme.h"
#include "ui.h"
#include "radio.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "kx3_iq_modal";

// Modal state - lazily created on first show.
static lv_obj_t *s_modal    = NULL;
static lv_obj_t *s_ta_vid   = NULL;
static lv_obj_t *s_ta_pid   = NULL;
static lv_obj_t *s_keyboard = NULL;
static bool      s_modal_open = false;

static void modal_close(void)
{
    if (!s_modal || !s_modal_open) return;
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    s_modal_open = false;
    ESP_LOGI(TAG, "Modal closed");
}

/* Parse 1-4 hex digits, with an optional "0x"/"0X" prefix. An empty or
 * whitespace-only field is 0 - which is a real, meaningful value here, not an
 * error: 0:0 is how "I/Q source not configured" is stored, and clearing the
 * dongle back to that is a legitimate thing for the operator to do.
 *
 * Returns false on anything that is not hex, or on more than 4 digits. A VID or
 * PID is 16 bits, so silently accepting "12345" would truncate a typo into a
 * plausible-looking wrong device. */
static bool parse_hex16(const char *in, uint16_t *out)
{
    if (!out) return false;
    while (in && *in && isspace((unsigned char)*in)) in++;
    if (!in) { *out = 0; return true; }
    if (in[0] == '0' && (in[1] == 'x' || in[1] == 'X')) in += 2;

    uint32_t v = 0;
    int      n = 0;
    for (; *in; in++) {
        if (isspace((unsigned char)*in)) break;
        int d;
        if      (*in >= '0' && *in <= '9') d = *in - '0';
        else if (*in >= 'a' && *in <= 'f') d = *in - 'a' + 10;
        else if (*in >= 'A' && *in <= 'F') d = *in - 'A' + 10;
        else return false;
        v = (v << 4) | (uint32_t)d;
        if (++n > 4) return false;
    }
    if (n == 0) { *out = 0; return true; }   /* "0x" alone reads as 0 */
    *out = (uint16_t)v;
    return true;
}

/* Uppercase in place, so what the operator typed is what gets logged and what
 * comes back up next time. */
static void to_upper_inplace(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

/* Persist and leave. Shared by Save and Auto-detect so both log the same way
 * and neither can drift from the other. */
static void persist_and_close(uint16_t vid, uint16_t pid, const char *toast)
{
    ESP_LOGI(TAG, "Save: I/Q dongle %04X:%04X%s", (unsigned)vid, (unsigned)pid,
             (vid | pid) ? "" : " (cleared - not configured)");
    radio_set_dongle_vid_pid(vid, pid);
    modal_close();
    ui_toast_ms(toast, 4000);
}

static void save_btn_cb(lv_event_t *e)
{
    (void)e;

    char vid_s[16] = {0}, pid_s[16] = {0};
    const char *vid_raw = lv_textarea_get_text(s_ta_vid);
    const char *pid_raw = lv_textarea_get_text(s_ta_pid);
    if (vid_raw) { strncpy(vid_s, vid_raw, sizeof(vid_s) - 1); to_upper_inplace(vid_s); }
    if (pid_raw) { strncpy(pid_s, pid_raw, sizeof(pid_s) - 1); to_upper_inplace(pid_s); }

    uint16_t vid = 0, pid = 0;
    if (!parse_hex16(vid_s, &vid) || !parse_hex16(pid_s, &pid)) {
        ESP_LOGW(TAG, "Save rejected: '%s' / '%s' is not 1-4 hex digits", vid_s, pid_s);
        /* Stay open. Closing here would discard the entry and look like the
         * keyboard ate it - the operator would tap Save again and be sent back
         * to the same place with nothing typed in. */
        ui_toast("Enter up to 4 hex digits, e.g. 0C46");
        return;
    }

    persist_and_close(vid, pid,
        (vid | pid) ? "I/Q source saved - unplug the dongle, plug it back in"
                    : "I/Q source cleared - the panadapter will stay silent");
}

/* The one-tap door for an operator who does not know their dongle's VID/PID -
 * which is everyone, and is exactly the case the manual entry cannot help with.
 * 0:0 is "not configured", the value audio.c treats as "adopt the next UAC
 * capture device that connects"; audio.c then persists whatever passes its
 * bit-depth and 48 kHz checks. Clearing two hex fields on a touch keyboard is
 * the step that was not happening on the bench (KX3, 2026-10-08), so it gets a
 * button instead. */
static void autodetect_btn_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_ta_vid, "");
    lv_textarea_set_text(s_ta_pid, "");
    persist_and_close(0, 0,
        "Auto-detect armed - unplug the dongle, plug it back in");
}

static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void ta_focused_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);

    if (!s_keyboard) return;

    /* Only one field shows the blinking cursor at a time. */
    lv_obj_t *other = (ta == s_ta_vid) ? s_ta_pid : s_ta_vid;
    lv_obj_remove_state(other, LV_STATE_FOCUSED);

    lv_keyboard_set_textarea(s_keyboard, ta);
    ui_osk_show(s_keyboard);
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void modal_build(void)
{
    if (s_modal) return;

    lv_obj_t *scr = lv_screen_active();

    s_modal = lv_obj_create(scr);
    lv_obj_set_size(s_modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_modal, UI_OPA_MODAL_SCRIM, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);
    lv_obj_set_style_radius(s_modal, 0, 0);
    lv_obj_set_style_pad_all(s_modal, 0, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel = lv_obj_create(s_modal);
    lv_obj_set_size(panel, 880, 540);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1c2128), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "KX3 I/Q dongle");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* Why the operator is being asked for this at all - a bare VID/PID pair
     * invites the reasonable question "which device is this?", and answering
     * it here is cheaper than a support round-trip. */
    lv_obj_t *why = lv_label_create(panel);
    lv_label_set_text(why, "A KX3 has no USB I/Q. Feed the \"RX I/Q\" jack\n"
                          "into a line-in USB dongle - put its VID:PID here.\n"
                          "The log names every USB audio device it can see.");
    lv_obj_set_style_text_color(why, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(why, &lv_font_montserrat_20, 0);
    lv_obj_align(why, LV_ALIGN_TOP_LEFT, 0, 50);

    lv_obj_t *vid_lbl = lv_label_create(panel);
    lv_label_set_text(vid_lbl, "USB VID");
    lv_obj_set_style_text_color(vid_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(vid_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(vid_lbl, LV_ALIGN_TOP_LEFT, 0, 140);

    s_ta_vid = lv_textarea_create(panel);
    lv_obj_set_size(s_ta_vid, 380, 60);
    lv_obj_align(s_ta_vid, LV_ALIGN_TOP_LEFT, 0, 170);
    lv_textarea_set_one_line(s_ta_vid, true);
    lv_textarea_set_max_length(s_ta_vid, 6);      /* "0x" + 4 digits */
    lv_textarea_set_placeholder_text(s_ta_vid, "0000");
    lv_obj_set_style_text_font(s_ta_vid, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_ta_vid);
    /* ⛔ FOCUSED *and* CLICKED, on every text field in the firmware. CLICKED is
     * not belt-and-braces: if LVGL still considers the field focused, a second
     * tap sends no FOCUSED event, the keyboard never comes back, and the dialog
     * is left looking alive with no way to finish typing. Samuel W7STF hit this
     * on all six modals. See ui_osk_show(). */
    lv_obj_add_event_cb(s_ta_vid, ta_focused_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_ta_vid, ta_focused_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *pid_lbl = lv_label_create(panel);
    lv_label_set_text(pid_lbl, "USB PID");
    lv_obj_set_style_text_color(pid_lbl, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(pid_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(pid_lbl, LV_ALIGN_TOP_RIGHT, -24, 140);

    s_ta_pid = lv_textarea_create(panel);
    lv_obj_set_size(s_ta_pid, 380, 60);
    lv_obj_align(s_ta_pid, LV_ALIGN_TOP_RIGHT, -24, 170);
    lv_textarea_set_one_line(s_ta_pid, true);
    lv_textarea_set_max_length(s_ta_pid, 6);
    lv_textarea_set_placeholder_text(s_ta_pid, "0000");
    lv_obj_set_style_text_font(s_ta_pid, &lv_font_montserrat_24, 0);
    ui_theme_style_textarea(s_ta_pid);
    lv_obj_add_event_cb(s_ta_pid, ta_focused_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_ta_pid, ta_focused_cb, LV_EVENT_CLICKED, NULL);

    /* Spells out the manual step. audio.c only ever runs open_and_start() from
     * a UAC connect event, so a VID/PID saved while the dongle is already
     * plugged in is not picked up until the device re-enumerates. Saying so
     * here is better than the operator saving, seeing no change, and saving
     * again. */
    lv_obj_t *note = lv_label_create(panel);
    lv_label_set_text(note, "Takes effect when the dongle is re-plugged.");
    lv_obj_set_style_text_color(note, lv_color_hex(UI_COLOR_ACCENT_GOLD), 0);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_20, 0);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 244);

    /* Spelled out so a blank field is not read as a mistake: blank = 0:0 =
     * "adopt the next dongle", the same thing the Auto-detect button does. */
    lv_obj_t *hint = lv_label_create(panel);
    lv_label_set_text(hint, "Blank fields mean auto-detect - or tap Auto-detect.");
    lv_obj_set_style_text_color(hint, lv_color_hex(UI_COLOR_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 274);

    /* ⛔ The button row sits at y=308, NOT pinned to the panel bottom. The hex
     * keyboard is 280 px tall at the bottom of the 720 px screen (top edge at
     * y=440) and the panel runs from y=24 to y=564, so the panel's bottom
     * ~100 px - where Save and Cancel used to live - are UNDER the keyboard the
     * moment a field is tapped to edit. Save was unreachable exactly when it
     * was needed, which is why the bench never saw a "Save:" line (KX3,
     * 2026-10-08). Same trap as ui_osk_show() documents for every text field. */
    lv_obj_t *auto_btn = lv_btn_create(panel);
    lv_obj_set_size(auto_btn, 260, 72);
    lv_obj_align(auto_btn, LV_ALIGN_TOP_LEFT, 0, 308);
    lv_obj_set_style_bg_color(auto_btn, lv_color_hex(0x1f6f8b), 0);
    lv_obj_set_style_border_color(auto_btn, lv_color_hex(0x3aa0c4), 0);
    lv_obj_set_style_border_width(auto_btn, 2, 0);
    lv_obj_set_style_radius(auto_btn, 8, 0);
    lv_obj_add_event_cb(auto_btn, autodetect_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *auto_btn_lbl = lv_label_create(auto_btn);
    lv_label_set_text(auto_btn_lbl, "Auto-detect");
    lv_obj_set_style_text_color(auto_btn_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(auto_btn_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(auto_btn_lbl);

    lv_obj_t *cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, 200, 72);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_MID, 0, 308);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x962020), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0xc04040), 0);
    lv_obj_set_style_border_width(cancel_btn, 2, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *save_btn = lv_btn_create(panel);
    lv_obj_set_size(save_btn, 200, 72);
    lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, 0, 308);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(0x2e8b3a), 0);
    lv_obj_set_style_border_color(save_btn, lv_color_hex(0x4caf50), 0);
    lv_obj_set_style_border_width(save_btn, 2, 0);
    lv_obj_set_style_radius(save_btn, 8, 0);
    lv_obj_add_event_cb(save_btn, save_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(save_lbl);

    /* Physical keyboard: Enter -> Save, Esc -> Cancel. */
    ui_kbd_set_buttons(save_btn, cancel_btn);

    s_keyboard = lv_keyboard_create(s_modal);
    ui_theme_style_keyboard(s_keyboard);
    lv_obj_set_size(s_keyboard, LV_PCT(100), 280);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* Hex pad, not the text keyboard. LVGL 9.2.2 has no built-in hex map -
     * its modes are TEXT_LOWER/UPPER, SPECIAL, NUMBER, USER_1..4 - so supply
     * one on USER_1. A VID/PID is 1-4 hex digits and nothing else, and the
     * default handler types every key here literally: none of these labels is
     * one of the control keys it intercepts by name except BACKSPACE and OK,
     * which is exactly what we want it to intercept.
     *
     * The theme's abc/Abc/ABC shift cycle is deliberately NOT attached. It only
     * understands the two TEXT modes and would fight a custom map. */
    static const char * const hex_map[] = {
        "1", "2", "3", "A", "B", "C", LV_SYMBOL_BACKSPACE, "\n",
        "4", "5", "6", "D", "E", "F", LV_SYMBOL_OK,        "\n",
        "7", "8", "9", "0", LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, "",
        NULL
    };
    /* ⛔ The ctrl map MUST NOT be NULL. LVGL 9.2.2's
     * lv_keyboard_update_ctrl_map() memcpy()s btn_cnt entries out of
     * kb_ctrl[mode] whenever popovers are off (the default), so a NULL map is a
     * load-access fault in lv_memcpy at modal-build time - and modal_build()
     * runs at boot, so the Tab5 reset-looped with a blank screen (KX3 bench,
     * 2026-10-06). One entry per button in hex_map: 7 + 7 + 7 = 21. Width 1
     * each; the four control keys get the theme's "checked" look like the
     * stock maps do. */
    #define HEX_KEY  (LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 1)
    #define HEX_CTRL (LV_KEYBOARD_CTRL_BUTTON_FLAGS | 1)
    static const lv_buttonmatrix_ctrl_t hex_ctrl[] = {
        HEX_KEY, HEX_KEY, HEX_KEY, HEX_KEY, HEX_KEY, HEX_KEY, HEX_CTRL,
        HEX_KEY, HEX_KEY, HEX_KEY, HEX_KEY, HEX_KEY, HEX_KEY, HEX_CTRL,
        HEX_KEY, HEX_KEY, HEX_KEY, HEX_KEY, HEX_CTRL, HEX_KEY, HEX_CTRL,
    };
    _Static_assert(sizeof(hex_ctrl) / sizeof(hex_ctrl[0]) == 21,
                   "hex_ctrl needs one entry per hex_map button");
    lv_keyboard_set_map(s_keyboard, LV_KEYBOARD_MODE_USER_1, hex_map, hex_ctrl);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    ESP_LOGI(TAG, "Modal built");
}

void kx3_iq_modal_init(void)
{
    modal_build();
}

void kx3_iq_modal_show(void)
{
    modal_build();  /* no-op if already built (idempotent via s_modal guard) */
    if (s_modal_open) return;

    uint16_t vid = 0, pid = 0;
    radio_iq_vid_pid(&vid, &pid);

    char buf[8];
    if (vid | pid) {
        snprintf(buf, sizeof(buf), "%04X", (unsigned)vid);
        lv_textarea_set_text(s_ta_vid, buf);
        snprintf(buf, sizeof(buf), "%04X", (unsigned)pid);
        lv_textarea_set_text(s_ta_pid, buf);
    } else {
        /* Unconfigured: show empty, not "0000". "0000" reads as a real answer
         * the operator typed; empty reads as the question still unanswered,
         * which is what it is. */
        lv_textarea_set_text(s_ta_vid, "");
        lv_textarea_set_text(s_ta_pid, "");
    }

    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_modal);
    s_modal_open = true;
    ESP_LOGI(TAG, "Modal shown (dongle %04X:%04X)", (unsigned)vid, (unsigned)pid);
}
