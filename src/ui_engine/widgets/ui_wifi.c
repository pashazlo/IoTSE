#include "ui_wifi.h"

#include <stdio.h>
#include <string.h>

#include "assets/ibm_vga_font.h"
#include "display.h"
#include "esp_mac.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_cursor.h"
#include "ui_keyboard.h"
#include "ui_popup.h"
#include "ui_screen.h"
#include "wifi_worker.h"

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)
#define WIFI_GLOBE_CENTER_X 307
#define WIFI_GLOBE_CENTER_Y 9
#define WIFI_GLOBE_RADIUS 6
#define WIFI_DETAILS_X 18
#define WIFI_DETAILS_Y 22
#define WIFI_DETAILS_W 284
#define WIFI_DETAILS_H 142
#define WIFI_VALUE_X 116
#define WIFI_VALUE_RIGHT 288
#define WIFI_SCROLL_START_MS 900U
#define WIFI_SCROLL_END_MS 1000U
#define WIFI_SCROLL_MS_PER_PIXEL 25U

static bool s_status_focused;
static bool s_details_open;
static uint8_t s_details_focus;

typedef struct {
    char text[40];
    uint32_t elapsed_ms;
    TickType_t last_tick;
    int16_t offset_px;
    bool active;
} wifi_detail_scroll_t;

static wifi_detail_scroll_t s_value_scroll;

void ui_wifi_request_disconnect(void)
{
    uint32_t request_id;
    if (!wifi_worker_is_connected()) return;
    if (wifi_worker_send_disconnect(&request_id) != ESP_OK) {
        ui_popup_show_error("Wi-Fi worker busy");
        return;
    }
    s_details_open = false;
    s_status_focused = false;
}

static bool screen_supports_wifi_status(void)
{
    ui_screen_t screen = ui_screen_get();
    return screen != UI_SCREEN_SPLASH &&
           screen != UI_SCREEN_FILE_VOLUMES &&
           screen != UI_SCREEN_FILE_BROWSER &&
           screen != UI_SCREEN_FILE_EDITOR &&
           screen != UI_SCREEN_WIFI_MONITOR;
}

static void draw_globe(gfx_canvas_t *canvas)
{
    gfx_canvas_draw_circle(canvas, WIFI_GLOBE_CENTER_X,
                           WIFI_GLOBE_CENTER_Y, WIFI_GLOBE_RADIUS, GFX_CYAN);
    gfx_canvas_draw_line(canvas, 302, 9, 312, 9, GFX_CYAN);
    gfx_canvas_draw_line(canvas, 307, 4, 307, 14, GFX_GREEN);
    gfx_canvas_draw_pixel(canvas, 304, 6, GFX_GREEN);
    gfx_canvas_draw_pixel(canvas, 310, 12, GFX_GREEN);
}

static void format_ipv4(uint32_t raw, char out[16])
{
    esp_ip4_addr_t address = {.addr = raw};
    snprintf(out, 16, IPSTR, IP2STR(&address));
}

static void update_value_scroll(const char *value)
{
    TickType_t now = xTaskGetTickCount();
    bool changed = strcmp(s_value_scroll.text, value) != 0;
    if (changed) {
        snprintf(s_value_scroll.text, sizeof(s_value_scroll.text), "%s", value);
        s_value_scroll.elapsed_ms = 0;
        s_value_scroll.offset_px = 0;
    }

    int16_t view_width = WIFI_VALUE_RIGHT - WIFI_VALUE_X;
    int16_t distance = gfx_canvas_measure_text_width(UI_FONT, value) - view_width;
    s_value_scroll.active = distance > 0;
    if (s_value_scroll.active && !changed) {
        uint64_t delta =
            (uint64_t)(TickType_t)(now - s_value_scroll.last_tick) *
            1000U / configTICK_RATE_HZ;
        uint32_t move_ms = (uint32_t)distance * WIFI_SCROLL_MS_PER_PIXEL;
        uint32_t cycle = WIFI_SCROLL_START_MS + move_ms + WIFI_SCROLL_END_MS;
        s_value_scroll.elapsed_ms =
            (s_value_scroll.elapsed_ms + delta) % cycle;
        uint32_t time = s_value_scroll.elapsed_ms;
        if (time <= WIFI_SCROLL_START_MS) s_value_scroll.offset_px = 0;
        else if (time >= WIFI_SCROLL_START_MS + move_ms)
            s_value_scroll.offset_px = distance;
        else s_value_scroll.offset_px =
            (time - WIFI_SCROLL_START_MS) / WIFI_SCROLL_MS_PER_PIXEL;
    } else if (!s_value_scroll.active) {
        s_value_scroll.elapsed_ms = 0;
        s_value_scroll.offset_px = 0;
    }
    s_value_scroll.last_tick = now;
}

static void draw_clipped_text(gfx_canvas_t *canvas, int16_t baseline,
                              const char *value, int16_t offset_px,
                              uint16_t color)
{
    int16_t pen_x = WIFI_VALUE_X - offset_px;
    const unsigned char *text = (const unsigned char *)value;
    for (; *text != '\0'; ++text) {
        if (*text < UI_FONT->first || *text > UI_FONT->last) continue;
        const gfx_glyph_t *glyph = &UI_FONT->glyphs[*text - UI_FONT->first];
        for (int yy = 0; yy < glyph->height; ++yy) {
            for (int xx = 0; xx < glyph->width; ++xx) {
                int16_t x = pen_x + glyph->xOffset + xx;
                int16_t y = baseline + glyph->yOffset + yy;
                if (x < WIFI_VALUE_X || x >= WIFI_VALUE_RIGHT ||
                    y < baseline - 11 || y > baseline + 3) continue;
                size_t bit = (size_t)yy * glyph->width + xx;
                if (UI_FONT->bitmap[glyph->bitmapOffset + bit / 8U] &
                    (0x80U >> (bit % 8U))) {
                    gfx_canvas_draw_pixel(canvas, x, y, color);
                }
            }
        }
        pen_x += glyph->xAdvance;
        if (pen_x >= WIFI_VALUE_RIGHT) break;
    }
}

static void draw_details(gfx_canvas_t *canvas, ui_cursor_t *shared_cursor)
{
    wifi_connection_snapshot_t snapshot;
    if (!wifi_worker_get_connection_snapshot(&snapshot) ||
        !snapshot.info.connected) {
        s_details_open = false;
        s_status_focused = false;
        return;
    }

    char ip[16], mask[16], gateway[16], sta_mac[18], bssid[18], rssi[12];
    format_ipv4(snapshot.info.ip, ip);
    format_ipv4(snapshot.info.netmask, mask);
    format_ipv4(snapshot.info.gateway, gateway);
    snprintf(sta_mac, sizeof(sta_mac), MACSTR, MAC2STR(snapshot.info.sta_mac));
    snprintf(bssid, sizeof(bssid), MACSTR, MAC2STR(snapshot.info.bssid));
    snprintf(rssi, sizeof(rssi), "%d dBm", (int)snapshot.info.rssi);

    gfx_canvas_fill_round_rect(canvas, WIFI_DETAILS_X, WIFI_DETAILS_Y,
                               WIFI_DETAILS_W, WIFI_DETAILS_H, 3, GFX_BLACK);
    gfx_canvas_draw_round_rect(canvas, WIFI_DETAILS_X, WIFI_DETAILS_Y,
                               WIFI_DETAILS_W, WIFI_DETAILS_H, 3, GFX_WHITE);
    gfx_canvas_draw_str(canvas, 28, 40, "Wi-Fi connection", UI_FONT, GFX_WHITE);
    gfx_canvas_draw_line(canvas, 284, 30, 294, 40, GFX_WHITE);
    gfx_canvas_draw_line(canvas, 294, 30, 284, 40, GFX_WHITE);
    static const char *labels[] = {"SSID", "IP", "MASK", "GW", "MAC", "BSSID", "RSSI"};
    const char *values[] = {snapshot.info.ssid, ip, mask, gateway,
                            sta_mac, bssid, rssi};
    if (s_details_focus < 7) update_value_scroll(values[s_details_focus]);
    for (size_t row = 0; row < 7; ++row) {
        int16_t y = 58 + (int16_t)row * 14;
        gfx_canvas_draw_str(canvas, 28, y, labels[row], UI_FONT, GFX_DARKGRAY);
        int16_t offset = row == s_details_focus ? s_value_scroll.offset_px : 0;
        draw_clipped_text(canvas, y, values[row], offset,
                          row == 6 ? GFX_GREEN : GFX_WHITE);
    }

    bool disconnect_selected = s_details_focus == 7;
    gfx_canvas_draw_round_rect(canvas, 116, 144, 172, 18, 3,
                               disconnect_selected ? GFX_WHITE : 0x4208);
    gfx_canvas_draw_str(canvas, 124, 157, "Disconnect", UI_FONT,
                         disconnect_selected ? GFX_WHITE : 0xBDF7);

    if (s_details_focus < 7) {
        int16_t y = 58 + (int16_t)s_details_focus * 14;
        ui_cursor_set_target(shared_cursor, 24, y - 12, 266, 15);
    } else if (disconnect_selected) {
        ui_cursor_set_target(shared_cursor, 114, 144, 176, 18);
    } else {
        ui_cursor_set_target(shared_cursor, 280, 26, 18, 18);
    }
    ui_cursor_step(shared_cursor);
    ui_cursor_draw(canvas, shared_cursor, GFX_WHITE);
}

void ui_wifi_draw(gfx_canvas_t *canvas, ui_cursor_t *shared_cursor)
{
    if (canvas == NULL || shared_cursor == NULL || !screen_supports_wifi_status() ||
        !wifi_worker_is_connected()) {
        s_status_focused = false;
        s_details_open = false;
        return;
    }

    draw_globe(canvas);
    if (s_status_focused && !s_details_open) {
        ui_cursor_set_target(shared_cursor, 298, 0, 18, 18);
        ui_cursor_step(shared_cursor);
        ui_cursor_draw(canvas, shared_cursor, GFX_WHITE);
    }
    if (s_details_open) draw_details(canvas, shared_cursor);
}

bool ui_wifi_handle_event(ui_event_t event)
{
    if (!screen_supports_wifi_status() || !wifi_worker_is_connected() ||
        ui_keyboard_is_open() || ui_popup_is_open()) {
        return false;
    }

    if (s_details_open) {
        uint8_t old_focus = s_details_focus;
        if (event == UI_EVT_LEFT) {
            s_details_open = false;
            s_status_focused = true;
        } else if (event == UI_EVT_RIGHT) {
            s_details_focus = 8;
        } else if (event == UI_EVT_UP) {
            s_details_focus = s_details_focus == 0 ? 7 :
                              s_details_focus == 8 ? 7 : s_details_focus - 1;
        } else if (event == UI_EVT_DOWN) {
            s_details_focus = s_details_focus >= 7 ? 0 : s_details_focus + 1;
        } else if (event == UI_EVT_SELECT && s_details_focus == 7) {
            ui_wifi_request_disconnect();
        } else if (event == UI_EVT_SELECT && s_details_focus == 8) {
            s_details_open = false;
            s_status_focused = true;
        }
        if (old_focus != s_details_focus)
            memset(&s_value_scroll, 0, sizeof(s_value_scroll));
        return true;
    }
    if (s_status_focused) {
        if (event == UI_EVT_SELECT) {
            s_details_open = true;
            s_details_focus = 0;
            memset(&s_value_scroll, 0, sizeof(s_value_scroll));
        } else if (event == UI_EVT_LEFT) {
            s_status_focused = false;
        }
        return true;
    }
    if (event == UI_EVT_RIGHT) {
        s_status_focused = true;
        return true;
    }
    return false;
}

bool ui_wifi_is_animating(void)
{
    return s_details_open && s_value_scroll.active;
}

bool ui_wifi_details_is_open(void)
{
    return s_details_open;
}

bool ui_wifi_has_focus(void)
{
    return s_status_focused || s_details_open;
}
