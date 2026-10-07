#include "ui_wifi_export.h"

#include <stdio.h>
#include <string.h>

#include "assets/ibm_vga_font.h"
#include "display.h"
#include "ui_popup.h"
#include "ui_cursor.h"
#include "ui_focus.h"
#include "ui_render.h"
#include "ui_screen.h"
#include "ui_wifi.h"
#include "wifi_capture_export.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)

static char s_url[80];
static struct {
    char text[80];
    uint32_t elapsed_ms;
    TickType_t last_tick;
    int16_t offset_px;
    bool active;
} s_scroll;

static void update_scroll(const char *text)
{
    TickType_t now = xTaskGetTickCount();
    bool changed = strcmp(s_scroll.text, text) != 0;
    if (changed) {
        snprintf(s_scroll.text, sizeof(s_scroll.text), "%s", text);
        s_scroll.elapsed_ms = 0;
        s_scroll.offset_px = 0;
    }
    int16_t distance = gfx_canvas_measure_text_width(UI_FONT, text) - 294;
    s_scroll.active = distance > 0;
    if (s_scroll.active && !changed) {
        uint32_t delta = (uint32_t)(TickType_t)(now - s_scroll.last_tick) *
                         1000U / configTICK_RATE_HZ;
        uint32_t move_ms = (uint32_t)distance * 25U;
        uint32_t cycle = 1000U + move_ms + 1200U;
        s_scroll.elapsed_ms = (s_scroll.elapsed_ms + delta) % cycle;
        if (s_scroll.elapsed_ms <= 1000U) s_scroll.offset_px = 0;
        else if (s_scroll.elapsed_ms >= 1000U + move_ms) s_scroll.offset_px = distance;
        else s_scroll.offset_px = (s_scroll.elapsed_ms - 1000U) / 25U;
    } else if (!s_scroll.active) {
        s_scroll.elapsed_ms = 0;
        s_scroll.offset_px = 0;
    }
    s_scroll.last_tick = now;
}

static void draw_clipped(gfx_canvas_t *canvas, int16_t baseline)
{
    int16_t pen_x = 10 - s_scroll.offset_px;
    const unsigned char *text = (const unsigned char *)s_scroll.text;
    for (; *text; ++text) {
        if (*text < UI_FONT->first || *text > UI_FONT->last) continue;
        const gfx_glyph_t *glyph = &UI_FONT->glyphs[*text - UI_FONT->first];
        for (int y = 0; y < glyph->height; ++y) for (int x = 0; x < glyph->width; ++x) {
            int16_t px = pen_x + glyph->xOffset + x;
            int16_t py = baseline + glyph->yOffset + y;
            size_t bit = (size_t)y * glyph->width + x;
            if (px >= 10 && px < 304 && py >= baseline - 11 && py <= baseline + 3 &&
                (UI_FONT->bitmap[glyph->bitmapOffset + bit / 8] & (0x80U >> (bit % 8))))
                gfx_canvas_draw_pixel(canvas, px, py, GFX_WHITE);
        }
        pen_x += glyph->xAdvance;
        if (pen_x >= 304) break;
    }
}

void ui_wifi_export_open(void)
{
    esp_err_t err = wifi_capture_export_start(s_url, sizeof(s_url));
    if (err != ESP_OK) {
        ui_popup_show_error("Connect Wi-Fi first");
        return;
    }
    ui_screen_set(UI_SCREEN_WIFI_EXPORT);
    ui_focus_reset(UI_FOCUS_WIFI_EXPORT);
    memset(&s_scroll, 0, sizeof(s_scroll));
}

void ui_wifi_export_handle_event(ui_event_t event, gfx_canvas_t *canvas)
{
    if (event == UI_EVT_UP || event == UI_EVT_DOWN) {
        ui_focus_move(UI_FOCUS_WIFI_EXPORT, 3, event);
        memset(&s_scroll, 0, sizeof(s_scroll));
        ui_render(canvas);
    } else if (event == UI_EVT_LEFT ||
               (event == UI_EVT_SELECT && ui_focus_get(UI_FOCUS_WIFI_EXPORT) == 2)) {
        wifi_capture_export_stop();
        ui_screen_set(UI_SCREEN_WIFI_MENU);
        ui_render(canvas);
    }
}

void ui_wifi_export_draw(gfx_canvas_t *canvas, ui_cursor_t *cursor)
{
    gfx_canvas_fill(canvas, GFX_BLACK);
    gfx_canvas_draw_str(canvas, 10, 12, "PCAP Web Export", UI_FONT, GFX_WHITE);
    gfx_canvas_draw_line(canvas, 0, 18, DISPLAY_WIDTH - 1, 18, GFX_WHITE);
    gfx_canvas_draw_str(canvas, 10, 39, "Open on your PC:", UI_FONT, GFX_DARKGRAY);

    const char *token = strstr(s_url, "?t=");
    char address[48];
    size_t address_len = token != NULL ? (size_t)(token - s_url) : strlen(s_url);
    if (address_len >= sizeof(address)) address_len = sizeof(address) - 1;
    memcpy(address, s_url, address_len);
    address[address_len] = '\0';
    char rows[3][80] = {{0}};
    snprintf(rows[0], sizeof(rows[0]), "%s", address);
    if (token != NULL) {
        snprintf(rows[1], sizeof(rows[1]), "Token: %s", token + 3);
    }
    snprintf(rows[2], sizeof(rows[2]), "< Stop export");
    uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_EXPORT);
    for (uint8_t i = 0; i < 3; ++i) {
        int16_t y = 68 + (int16_t)i * 30;
        if (i == selected) {
            update_scroll(rows[i]);
            draw_clipped(canvas, y);
            if (!ui_wifi_has_focus()) {
                ui_cursor_set_target(cursor, 7, y - 13, 300, 17);
                ui_cursor_step(cursor);
                ui_cursor_draw(canvas, cursor, GFX_WHITE);
            }
        } else {
            gfx_canvas_draw_str(canvas, 10, y, rows[i], UI_FONT,
                                i == 1 ? GFX_YELLOW : GFX_DARKGRAY);
        }
    }
    gfx_canvas_draw_str(canvas, 10, 154, "Keep this screen open", UI_FONT, GFX_DARKGRAY);
}

bool ui_wifi_export_needs_tick(void)
{
    return s_scroll.active;
}
