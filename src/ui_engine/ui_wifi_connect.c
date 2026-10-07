#include "ui_wifi_connect.h"
#include "ui_wifi.h"
#include "ui_wifi_monitor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/ibm_vga_font.h"
#include "display.h"
#include "ui_cursor.h"
#include "ui_focus.h"
#include "ui_keyboard.h"
#include "ui_popup.h"
#include "ui_render.h"
#include "ui_screen.h"
#include "wifi_worker.h"
#include "wifi_port_scan.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)
#define WIFI_VISIBLE_ROWS 7U
#define WIFI_NAME_X 10
#define WIFI_NAME_START_PAUSE_MS 1000U
#define WIFI_NAME_END_PAUSE_MS 1200U
#define WIFI_NAME_MS_PER_PIXEL 25U
#define WIFI_TEXT_ASCENT 11
#define WIFI_TEXT_DESCENT 3
#define WIFI_REFRESH_MS 5000U

typedef enum {
    WIFI_UI_IDLE = 0,
    WIFI_UI_STARTING,
    WIFI_UI_SCANNING,
    WIFI_UI_CONNECTING,
    WIFI_UI_SETTINGS,
} wifi_ui_phase_t;

static wifi_ui_phase_t s_phase;
static uint32_t s_request_id;
static wifi_scan_snapshot_t s_networks;
static uint16_t s_top;
static char s_selected_ssid[WIFI_SSID_MAX_LEN + 1];
static uint8_t s_selected_bssid[6];
static const char *s_status;
static bool s_refresh_scan;
static TickType_t s_last_scan_tick;
static uint8_t s_preserved_bssid[6];
static bool s_have_preserved_bssid;
static bool s_connecting_with_saved_credentials;
static bool s_requested_auto_connect;

typedef struct {
    bool active;
    bool paused;
    uint8_t selected;
    char text[WIFI_SSID_MAX_LEN + 1];
    int16_t view_width;
    int16_t offset_px;
    TickType_t last_tick;
    uint32_t elapsed_ms;
} wifi_name_scroll_t;

static wifi_name_scroll_t s_name_scroll;

static int compare_rssi_desc(const void *left, const void *right)
{
    const wifi_ap_info_t *a = left;
    const wifi_ap_info_t *b = right;
    return (int)b->rssi - (int)a->rssi;
}

static void start_scan(bool refresh)
{
    wifi_scan_options_t options = {
        .channel = 0,
        .show_hidden = false,
        .passive = false,
        .dwell_ms = 0,
    };
    if (wifi_worker_send_scan(&options, &s_request_id) != ESP_OK) {
        s_phase = WIFI_UI_IDLE;
        if (ui_screen_get() != UI_SCREEN_WIFI_NETWORKS)
            wifi_worker_resume_auto_connect();
        ui_popup_show_error("Wi-Fi worker busy");
        return;
    }
    s_phase = WIFI_UI_SCANNING;
    s_refresh_scan = refresh;
    if (!refresh) s_status = "Scanning...";
}

void ui_wifi_connect_begin(void)
{
    if (s_phase != WIFI_UI_IDLE) return;

    /* Networks owns the foreground radio session until the user leaves it. */
    wifi_worker_suspend_auto_connect();

    if (wifi_worker_get_state() == WIFI_WORKER_STATE_OFF ||
        wifi_worker_get_state() == WIFI_WORKER_STATE_ERROR) {
        if (wifi_worker_send_start(&s_request_id) != ESP_OK) {
            wifi_worker_resume_auto_connect();
            ui_popup_show_error("Wi-Fi worker busy");
            return;
        }
        s_phase = WIFI_UI_STARTING;
        s_status = "Starting...";
    } else {
        start_scan(false);
    }
}

static void connect_selected(const char *password)
{
    if (wifi_worker_send_connect(
            s_selected_ssid, password, s_selected_bssid,
            &s_request_id) != ESP_OK) {
        ui_popup_show_error("Connect queue failed");
        return;
    }
    s_connecting_with_saved_credentials = false;
    s_phase = WIFI_UI_CONNECTING;
    s_status = "Connecting...";
}

static bool connect_selected_with_saved_credentials(void)
{
    esp_err_t err = wifi_worker_send_connect_saved(
        s_selected_ssid, s_selected_bssid, &s_request_id);
    if (err != ESP_OK) return false;

    s_connecting_with_saved_credentials = true;
    s_phase = WIFI_UI_CONNECTING;
    s_status = "Connecting (saved)...";
    return true;
}

static bool snapshot_selected_network(void)
{
    uint16_t count = s_networks.scan.count;
    uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_NETWORKS);
    if (count == 0 || selected >= count) return false;

    const wifi_ap_info_t *ap = &s_networks.scan.records[selected];
    snprintf(s_selected_ssid, sizeof(s_selected_ssid), "%s", ap->ssid);
    memcpy(s_selected_bssid, ap->bssid, sizeof(s_selected_bssid));
    return true;
}

void ui_wifi_connect_handle_keyboard_result(void)
{
    if (ui_keyboard_was_confirmed()) {
        connect_selected(ui_keyboard_get_text());
    }
}

void ui_wifi_connect_handle_event(ui_event_t event, gfx_canvas_t *canvas)
{
    if (s_phase != WIFI_UI_IDLE) return;

    uint16_t count = s_networks.scan.count;
    if (event == UI_EVT_LEFT) {
        wifi_worker_resume_auto_connect();
        ui_screen_set(UI_SCREEN_WIFI_MENU);
    } else if ((event == UI_EVT_UP || event == UI_EVT_DOWN) && count > 0) {
        ui_focus_move(UI_FOCUS_WIFI_NETWORKS, (uint8_t)count, event);
    } else if (event == UI_EVT_CONTEXT && snapshot_selected_network()) {
        bool auto_connect = false;
        bool saved = wifi_worker_get_saved_network_state(
            s_selected_ssid,
            &auto_connect
        );
        ui_popup_open_wifi_network(
            s_selected_ssid,
            saved,
            auto_connect
        );
    } else if (event == UI_EVT_SELECT && count > 0) {
        if (!snapshot_selected_network()) return;
        uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_NETWORKS);
        const wifi_ap_info_t *ap = &s_networks.scan.records[selected];

        wifi_connection_snapshot_t connection = {0};
        if (wifi_worker_get_connection_snapshot(&connection) &&
            connection.info.connected &&
            strcmp(connection.info.ssid, s_selected_ssid) == 0) {
            s_status = "Connected";
        } else if (ap->security == WIFI_SECURITY_OPEN) {
            connect_selected("");
        } else if (wifi_worker_has_saved_network(s_selected_ssid)) {
            if (!connect_selected_with_saved_credentials()) {
                ui_popup_show_error("Saved connect busy");
            }
        } else {
            ui_keyboard_open_prompt(
                "Wi-Fi password", "", WIFI_PASSWORD_MAX_LEN, true);
        }
    }
    ui_render(canvas);
}

bool ui_wifi_connect_handle_popup_result(ui_popup_result_t result)
{
    if (result < UI_POPUP_WIFI_AUTO_TOGGLE ||
        result > UI_POPUP_WIFI_WEB) {
        return false;
    }

    if (result == UI_POPUP_WIFI_WEB) {
        ui_popup_show_message("Web UI", "Coming in a later", "release");
        return true;
    }
    bool auto_connect = false;
    bool saved = wifi_worker_get_saved_network_state(
        s_selected_ssid,
        &auto_connect
    );

    if (result == UI_POPUP_WIFI_AUTO_TOGGLE) {
        if (!saved) {
            ui_popup_show_message("Auto-connect", "Connect and save", "network first");
            return true;
        }
        s_requested_auto_connect = !auto_connect;
        esp_err_t err = wifi_worker_send_set_auto_connect(
            s_selected_ssid,
            s_requested_auto_connect,
            &s_request_id
        );
        if (err != ESP_OK) {
            ui_popup_show_error("Worker busy");
        } else {
            s_phase = WIFI_UI_SETTINGS;
            s_status = "Saving setting...";
        }
        return true;
    }

    if (result == UI_POPUP_WIFI_FORGET) {
        if (!saved) {
            ui_popup_show_message("Forget network", "No saved password", "for this network");
            return true;
        }
        esp_err_t err = wifi_worker_send_forget_network(
            s_selected_ssid,
            &s_request_id
        );
        if (err != ESP_OK) {
            ui_popup_show_error("Worker busy");
        } else {
            s_phase = WIFI_UI_SETTINGS;
            s_status = "Forgetting...";
        }
        return true;
    }

    return true;
}

void ui_wifi_connect_poll(gfx_canvas_t *canvas)
{
    wifi_worker_result_t result;
    while (wifi_worker_receive_result(&result)) {
        if (result.request_id != s_request_id) {
            (void)ui_wifi_monitor_handle_worker_result(&result);
            continue;
        }

        if (result.type == WIFI_WORKER_RESULT_STARTED &&
            s_phase == WIFI_UI_STARTING) {
            start_scan(false);
        } else if (result.type == WIFI_WORKER_RESULT_SCAN_READY &&
                   s_phase == WIFI_UI_SCANNING) {
            if (wifi_worker_get_scan_snapshot(&s_networks)) {
                qsort(s_networks.scan.records, s_networks.scan.count,
                      sizeof(s_networks.scan.records[0]), compare_rssi_desc);
                s_phase = WIFI_UI_IDLE;
                s_status = s_networks.scan.count > 0 ? NULL : "No networks";
                if (s_refresh_scan && s_have_preserved_bssid) {
                    for (uint16_t i = 0; i < s_networks.scan.count; ++i) {
                        if (memcmp(s_networks.scan.records[i].bssid,
                                   s_preserved_bssid, 6) == 0) {
                            ui_focus_set(UI_FOCUS_WIFI_NETWORKS, (uint8_t)i);
                            break;
                        }
                    }
                } else {
                    s_top = 0;
                    ui_focus_reset(UI_FOCUS_WIFI_NETWORKS);
                }
                s_last_scan_tick = xTaskGetTickCount();
                s_refresh_scan = false;
                ui_screen_set(UI_SCREEN_WIFI_NETWORKS);
            }
        } else if (result.type == WIFI_WORKER_RESULT_CONNECTED &&
                   s_phase == WIFI_UI_CONNECTING) {
            s_phase = WIFI_UI_IDLE;
            s_connecting_with_saved_credentials = false;
            s_status = "Connected";
        } else if (result.type == WIFI_WORKER_RESULT_AUTO_CONNECT_UPDATED &&
                   s_phase == WIFI_UI_SETTINGS) {
            s_phase = WIFI_UI_IDLE;
            s_status = s_requested_auto_connect
                ? "Auto-connect enabled" : "Auto-connect disabled";
        } else if (result.type == WIFI_WORKER_RESULT_NETWORK_FORGOTTEN &&
                   s_phase == WIFI_UI_SETTINGS) {
            s_phase = WIFI_UI_IDLE;
            s_status = "Network forgotten";
        } else if (result.type == WIFI_WORKER_RESULT_ERROR) {
            s_phase = WIFI_UI_IDLE;
            if (ui_screen_get() != UI_SCREEN_WIFI_NETWORKS)
                wifi_worker_resume_auto_connect();
            if (result.command == WIFI_WORKER_CMD_SET_AUTO_CONNECT ||
                result.command == WIFI_WORKER_CMD_FORGET_NETWORK) {
                s_status = "Setting failed";
                ui_popup_show_error("Setting failed");
            } else if (s_connecting_with_saved_credentials &&
                       result.failure_class == WIFI_CONN_FAIL_AUTH) {
                s_connecting_with_saved_credentials = false;
                s_status = "Authentication failed";
                ui_keyboard_open_prompt(
                    "Wi-Fi password", "", WIFI_PASSWORD_MAX_LEN, true);
            } else {
                s_connecting_with_saved_credentials = false;
                s_status = "Connection failed";
                ui_popup_show_error("Wi-Fi operation failed");
            }
        }
        if (canvas != NULL) ui_render(canvas);
    }

    if (ui_screen_get() == UI_SCREEN_WIFI_NETWORKS &&
        s_phase == WIFI_UI_IDLE && !ui_keyboard_is_open() &&
        !ui_popup_is_open() && !wifi_worker_request_pending() &&
        !wifi_port_scan_is_running()) {
        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed = (uint32_t)(TickType_t)(now - s_last_scan_tick) *
                           1000U / configTICK_RATE_HZ;
        if (elapsed >= WIFI_REFRESH_MS) {
            uint16_t count = s_networks.scan.count;
            uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_NETWORKS);
            s_have_preserved_bssid = count > 0 && selected < count;
            if (s_have_preserved_bssid)
                memcpy(s_preserved_bssid,
                       s_networks.scan.records[selected].bssid, 6);
            start_scan(true);
        }
    }
}

static uint16_t rssi_color(int8_t rssi)
{
    switch (wifi_rssi_quality(rssi)) {
        case WIFI_RSSI_GOOD: return GFX_GREEN;
        case WIFI_RSSI_FAIR: return GFX_YELLOW;
        default: return GFX_RED;
    }
}

static void update_name_scroll(
    const wifi_ap_info_t *ap,
    uint8_t selected,
    int16_t view_width
)
{
    const char *text = ap != NULL && ap->ssid[0] != '\0'
        ? ap->ssid
        : "<hidden>";
    TickType_t now = xTaskGetTickCount();
    bool paused = ui_popup_is_open() || ui_keyboard_is_open();
    bool changed = selected != s_name_scroll.selected ||
        view_width != s_name_scroll.view_width ||
        strcmp(text, s_name_scroll.text) != 0;

    if (changed) {
        snprintf(s_name_scroll.text, sizeof(s_name_scroll.text), "%s", text);
        s_name_scroll.selected = selected;
        s_name_scroll.view_width = view_width;
        s_name_scroll.elapsed_ms = 0;
        s_name_scroll.offset_px = 0;
    }

    int16_t distance = gfx_canvas_measure_text_width(UI_FONT, text) - view_width;
    s_name_scroll.active = ap != NULL && distance > 0;
    if (s_name_scroll.active) {
        uint32_t move_ms = (uint32_t)distance * WIFI_NAME_MS_PER_PIXEL;
        uint32_t cycle_ms = WIFI_NAME_START_PAUSE_MS + move_ms +
                            WIFI_NAME_END_PAUSE_MS;
        if (!changed && !paused && !s_name_scroll.paused) {
            uint64_t delta_ms =
                (uint64_t)(TickType_t)(now - s_name_scroll.last_tick) *
                1000U / configTICK_RATE_HZ;
            s_name_scroll.elapsed_ms =
                (s_name_scroll.elapsed_ms + delta_ms) % cycle_ms;
        }
        uint32_t time = s_name_scroll.elapsed_ms;
        if (time <= WIFI_NAME_START_PAUSE_MS) {
            s_name_scroll.offset_px = 0;
        } else if (time >= WIFI_NAME_START_PAUSE_MS + move_ms) {
            s_name_scroll.offset_px = distance;
        } else {
            s_name_scroll.offset_px =
                (time - WIFI_NAME_START_PAUSE_MS) / WIFI_NAME_MS_PER_PIXEL;
        }
    } else {
        s_name_scroll.elapsed_ms = 0;
        s_name_scroll.offset_px = 0;
    }
    s_name_scroll.paused = paused;
    s_name_scroll.last_tick = now;
}

static void draw_scrolling_name(gfx_canvas_t *canvas, int16_t baseline)
{
    int16_t pen_x = WIFI_NAME_X - s_name_scroll.offset_px;
    const unsigned char *text =
        (const unsigned char *)s_name_scroll.text;

    for (; *text != '\0'; ++text) {
        if (*text < UI_FONT->first || *text > UI_FONT->last) continue;
        const gfx_glyph_t *glyph = &UI_FONT->glyphs[*text - UI_FONT->first];
        for (int yy = 0; yy < glyph->height; ++yy) {
            int16_t y = baseline + glyph->yOffset + yy;
            for (int xx = 0; xx < glyph->width; ++xx) {
                int16_t x = pen_x + glyph->xOffset + xx;
                if (x < WIFI_NAME_X ||
                    x >= WIFI_NAME_X + s_name_scroll.view_width ||
                    y < baseline - WIFI_TEXT_ASCENT ||
                    y > baseline + WIFI_TEXT_DESCENT) continue;
                size_t bit = (size_t)yy * glyph->width + xx;
                if (UI_FONT->bitmap[glyph->bitmapOffset + bit / 8U] &
                    (0x80U >> (bit % 8U))) {
                    gfx_canvas_draw_pixel(canvas, x, y, GFX_WHITE);
                }
            }
        }
        pen_x += glyph->xAdvance;
        if (pen_x >= WIFI_NAME_X + s_name_scroll.view_width) break;
    }
}

static void make_short_name(
    char *out,
    size_t out_size,
    const char *ssid,
    int16_t max_width
)
{
    snprintf(out, out_size, "%s", ssid[0] != '\0' ? ssid : "<hidden>");
    if (gfx_canvas_measure_text_width(UI_FONT, out) <= max_width) return;

    const int16_t ellipsis_width = gfx_canvas_measure_text_width(UI_FONT, "...");
    while (out[0] != '\0' &&
           gfx_canvas_measure_text_width(UI_FONT, out) + ellipsis_width >
               max_width) {
        out[strlen(out) - 1U] = '\0';
    }
    if (strlen(out) + 3U < out_size) strcat(out, "...");
}

void ui_wifi_connect_draw(gfx_canvas_t *canvas, ui_cursor_t *cursor)
{
    gfx_canvas_fill(canvas, GFX_BLACK);
    gfx_canvas_draw_str(canvas, 10, 12, "Wi-Fi networks", UI_FONT, GFX_WHITE);
    gfx_canvas_draw_line(canvas, 0, 18, DISPLAY_WIDTH - 1, 18, GFX_WHITE);

    uint16_t count = s_networks.scan.count;
    uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_NETWORKS);
    if (count > 0 && selected >= count) selected = (uint8_t)(count - 1U);
    if (selected < s_top) s_top = selected;
    if (selected >= s_top + WIFI_VISIBLE_ROWS)
        s_top = selected - WIFI_VISIBLE_ROWS + 1U;

    if (count > 0) {
        const wifi_ap_info_t *selected_ap = &s_networks.scan.records[selected];
        char selected_rssi[9];
        snprintf(selected_rssi, sizeof(selected_rssi), "%d dBm",
                 (int)selected_ap->rssi);
        int16_t selected_rssi_width =
            gfx_canvas_measure_text_width(UI_FONT, selected_rssi);
        update_name_scroll(
            selected_ap,
            selected,
            DISPLAY_WIDTH - selected_rssi_width - WIFI_NAME_X - 14
        );
    } else {
        memset(&s_name_scroll, 0, sizeof(s_name_scroll));
    }

    for (uint16_t row = 0; row < WIFI_VISIBLE_ROWS; ++row) {
        uint16_t index = s_top + row;
        if (index >= count) break;
        const wifi_ap_info_t *ap = &s_networks.scan.records[index];
        int16_t y = 36 + (int16_t)row * 18;
        char rssi[9];
        snprintf(rssi, sizeof(rssi), "%d dBm", (int)ap->rssi);
        int16_t rssi_width = gfx_canvas_measure_text_width(UI_FONT, rssi);
        int16_t max_name_width = DISPLAY_WIDTH - rssi_width - 24;
        char name[WIFI_SSID_MAX_LEN + 1];
        make_short_name(name, sizeof(name), ap->ssid, max_name_width);

        if (index == selected && s_name_scroll.active) {
            draw_scrolling_name(canvas, y);
        } else {
            gfx_canvas_draw_str(canvas, WIFI_NAME_X, y, name,
                                UI_FONT, GFX_WHITE);
        }
        gfx_canvas_draw_str(canvas, DISPLAY_WIDTH - rssi_width - 4, y,
                            rssi, UI_FONT, rssi_color(ap->rssi));
        if (index == selected && !ui_wifi_has_focus()) {
            ui_cursor_set_target(cursor, 7, y - 13,
                                 DISPLAY_WIDTH - 14, 17);
            ui_cursor_step(cursor);
            ui_cursor_draw(canvas, cursor, GFX_WHITE);
        }
    }

    if (s_status != NULL)
        gfx_canvas_draw_str(canvas, 10, DISPLAY_HEIGHT - 5,
                            s_status, UI_FONT, GFX_DARKGRAY);
}

bool ui_wifi_connect_pending(void)
{
    return s_phase != WIFI_UI_IDLE;
}

bool ui_wifi_connect_needs_tick(void)
{
    return s_name_scroll.active;
}
