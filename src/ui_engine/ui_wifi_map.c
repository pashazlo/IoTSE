#include "ui_wifi_map.h"

#include <stdio.h>
#include <string.h>

#include "assets/ibm_vga_font.h"
#include "display.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_focus.h"
#include "ui_render.h"
#include "ui_screen.h"
#include "ui_popup.h"
#include "ui_wifi.h"
#include "wifi_port_scan.h"
#include "wifi_worker.h"

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)
#define MAP_ACTION_COUNT 3U
#define MAP_TEXT_MAX_WIDTH 300
#define MAP_RESULT_VISIBLE_ROWS 4U
#define MAP_HEADING_VIEW_WIDTH 300
#define MAP_HEADING_START_MS 1000U
#define MAP_HEADING_END_MS 1200U
#define MAP_HEADING_MS_PER_PIXEL 25U

static const char *TAG = "UI_WIFI_MAP";

static wifi_port_scan_result_t s_result;
static bool s_have_result;
static char s_status[48] = "Select scan profile";
static uint16_t s_last_scanned = UINT16_MAX;
static uint16_t s_last_port = UINT16_MAX;

typedef struct {
    char text[64];
    int16_t offset_px;
    uint32_t elapsed_ms;
    TickType_t last_tick;
    bool active;
} map_heading_scroll_t;

static map_heading_scroll_t s_heading_scroll;

static void update_heading_scroll(const char *text, bool focused)
{
    TickType_t now = xTaskGetTickCount();
    bool changed = strcmp(s_heading_scroll.text, text) != 0;
    if (changed || !focused) {
        snprintf(s_heading_scroll.text, sizeof(s_heading_scroll.text), "%s",
                 text);
        s_heading_scroll.offset_px = 0;
        s_heading_scroll.elapsed_ms = 0U;
    }
    int16_t distance = (int16_t)(gfx_canvas_measure_text_width(UI_FONT, text) -
                                 MAP_HEADING_VIEW_WIDTH);
    s_heading_scroll.active = focused && distance > 0;
    if (s_heading_scroll.active && !changed) {
        uint32_t delta_ms = (uint32_t)(
            (uint64_t)(TickType_t)(now - s_heading_scroll.last_tick) * 1000U /
            configTICK_RATE_HZ);
        uint32_t move_ms = (uint32_t)distance * MAP_HEADING_MS_PER_PIXEL;
        uint32_t cycle_ms = MAP_HEADING_START_MS + move_ms +
                            MAP_HEADING_END_MS;
        s_heading_scroll.elapsed_ms =
            (s_heading_scroll.elapsed_ms + delta_ms) % cycle_ms;
        if (s_heading_scroll.elapsed_ms <= MAP_HEADING_START_MS) {
            s_heading_scroll.offset_px = 0;
        } else if (s_heading_scroll.elapsed_ms >=
                   MAP_HEADING_START_MS + move_ms) {
            s_heading_scroll.offset_px = distance;
        } else {
            s_heading_scroll.offset_px = (int16_t)(
                (s_heading_scroll.elapsed_ms - MAP_HEADING_START_MS) /
                MAP_HEADING_MS_PER_PIXEL);
        }
    }
    s_heading_scroll.last_tick = now;
}

static void draw_heading_clipped(gfx_canvas_t *canvas, int16_t baseline,
                                 const char *text, int16_t offset_px)
{
    int16_t pen_x = (int16_t)(10 - offset_px);
    const unsigned char *cursor = (const unsigned char *)text;
    for (; *cursor != '\0'; ++cursor) {
        if (*cursor < UI_FONT->first || *cursor > UI_FONT->last) continue;
        const gfx_glyph_t *glyph = &UI_FONT->glyphs[*cursor - UI_FONT->first];
        for (int yy = 0; yy < glyph->height; ++yy) {
            for (int xx = 0; xx < glyph->width; ++xx) {
                int16_t x = (int16_t)(pen_x + glyph->xOffset + xx);
                if (x < 10 || x >= 10 + MAP_HEADING_VIEW_WIDTH) continue;
                size_t bit = (size_t)yy * glyph->width + xx;
                if (UI_FONT->bitmap[glyph->bitmapOffset + bit / 8U] &
                    (0x80U >> (bit % 8U))) {
                    gfx_canvas_draw_pixel(canvas, x,
                        (int16_t)(baseline + glyph->yOffset + yy), GFX_WHITE);
                }
            }
        }
        pen_x = (int16_t)(pen_x + glyph->xAdvance);
        if (pen_x >= 10 + MAP_HEADING_VIEW_WIDTH) break;
    }
}

static void sort_probes_for_display(wifi_port_scan_result_t *result)
{
    if (result == NULL) return;
    for (uint8_t i = 1U; i < result->probe_count; ++i) {
        wifi_probe_result_t value = result->probes[i];
        uint8_t position = i;
        while (position > 0U) {
            const wifi_probe_result_t *previous =
                &result->probes[position - 1U];
            bool value_open = value.state == WIFI_PROBE_OPEN;
            bool previous_open = previous->state == WIFI_PROBE_OPEN;
            bool comes_first = value_open != previous_open
                ? value_open
                : value.port < previous->port;
            if (!comes_first) break;
            result->probes[position] = *previous;
            --position;
        }
        result->probes[position] = value;
    }
}

static const wifi_banner_observation_t *find_banner(uint16_t port)
{
    for (uint8_t i = 0U; i < s_result.banner_count; ++i) {
        if (s_result.banners[i].port == port) return &s_result.banners[i];
    }
    return NULL;
}

static uint16_t probe_color(wifi_probe_state_t state)
{
    switch (state) {
    case WIFI_PROBE_OPEN: return GFX_GREEN;
    case WIFI_PROBE_FILTERED: return GFX_YELLOW;
    case WIFI_PROBE_ERROR: return GFX_RED;
    case WIFI_PROBE_CLOSED:
    default: return GFX_DARKGRAY;
    }
}

static void fit_text(char *text, size_t size, int16_t max_width)
{
    if (text == NULL || size == 0 ||
        gfx_canvas_measure_text_width(UI_FONT, text) <= max_width) {
        return;
    }

    size_t length = strlen(text);
    while (length > 3U) {
        --length;
        text[length] = '\0';
        if (gfx_canvas_measure_text_width(UI_FONT, text) <= max_width) break;
    }
    if (length >= 3U) {
        text[length - 3U] = '.';
        text[length - 2U] = '.';
        text[length - 1U] = '.';
    }
}

static void format_ipv4(uint32_t raw, char out[16])
{
    esp_ip4_addr_t address = {.addr = raw};
    snprintf(out, 16, IPSTR, IP2STR(&address));
}

static void format_open_ports(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) return;
    snprintf(out, out_size, "Open:");
    if (s_result.open_count == 0) {
        snprintf(out, out_size, "Open: none");
        return;
    }

    for (uint8_t i = 0; i < s_result.open_count; ++i) {
        char candidate[80];
        snprintf(candidate, sizeof(candidate), "%s %u", out,
                 (unsigned)s_result.open_ports[i]);
        if (gfx_canvas_measure_text_width(UI_FONT, candidate) <=
            MAP_TEXT_MAX_WIDTH) {
            snprintf(out, out_size, "%s", candidate);
            continue;
        }
        if (strlen(out) + 3U < out_size) strcat(out, "...");
        break;
    }
}

static void set_start_error(esp_err_t error)
{
    if (error == ESP_ERR_INVALID_STATE && wifi_port_scan_is_running()) {
        snprintf(s_status, sizeof(s_status), "Scanner already running");
    } else if (error == ESP_ERR_TIMEOUT) {
        snprintf(s_status, sizeof(s_status), "Scanner queue full");
    } else {
        snprintf(s_status, sizeof(s_status), "Scan unavailable");
    }
}

static void start_scan(wifi_port_scan_profile_t profile)
{
    wifi_connection_snapshot_t connection = {0};
    if (!wifi_worker_get_connection_snapshot(&connection) ||
        !connection.info.connected || connection.info.gateway == 0) {
        snprintf(s_status, sizeof(s_status), "Connect Wi-Fi first");
        return;
    }

    esp_err_t error = wifi_port_scan_start_profile(
        connection.info.ssid,
        profile
    );
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Port scan start failed: %s", esp_err_to_name(error));
        set_start_error(error);
        return;
    }

    s_have_result = false;
    s_last_scanned = UINT16_MAX;
    s_last_port = UINT16_MAX;
    snprintf(s_status, sizeof(s_status), "Starting scan...");
}

void ui_wifi_map_open(void)
{
    ui_screen_set(UI_SCREEN_WIFI_MAP);
    ui_focus_reset(UI_FOCUS_WIFI_MAP);
    if (wifi_port_scan_is_running()) {
        snprintf(s_status, sizeof(s_status), "Scan in progress...");
    } else if (!s_have_result) {
        snprintf(s_status, sizeof(s_status), "Select scan profile");
    }
}

void ui_wifi_map_handle_event(ui_event_t event, gfx_canvas_t *canvas)
{
    if (s_have_result && !wifi_port_scan_is_running()) {
        if (event == UI_EVT_LEFT) {
            s_have_result = false;
            ui_focus_reset(UI_FOCUS_WIFI_MAP);
            snprintf(s_status, sizeof(s_status), "Select scan profile");
        } else if ((event == UI_EVT_UP || event == UI_EVT_DOWN) &&
                   s_result.probe_count > 0U) {
            ui_focus_move(UI_FOCUS_WIFI_MAP,
                          (uint8_t)(s_result.probe_count + 1U), event);
        } else if (event == UI_EVT_SELECT && s_result.probe_count > 0U) {
            uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_MAP);
            if (selected == 0U) {
                ui_render(canvas);
                return;
            }
            if (selected > s_result.probe_count) selected = 1U;
            const wifi_probe_result_t *probe = &s_result.probes[selected - 1U];
            ui_popup_open_wifi_map_port(probe, find_banner(probe->port));
        }
        ui_render(canvas);
        return;
    }

    if (event == UI_EVT_UP || event == UI_EVT_DOWN) {
        ui_focus_move(UI_FOCUS_WIFI_MAP, MAP_ACTION_COUNT, event);
    } else if (event == UI_EVT_LEFT) {
        ui_screen_set(UI_SCREEN_WIFI_MENU);
    } else if (event == UI_EVT_SELECT) {
        uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_MAP);
        if (selected == 0) {
            start_scan(WIFI_PORT_SCAN_PROFILE_QUICK);
        } else if (selected == 1) {
            start_scan(WIFI_PORT_SCAN_PROFILE_COMMON);
        } else if (wifi_port_scan_is_running()) {
            if (wifi_port_scan_cancel() == ESP_OK) {
                snprintf(s_status, sizeof(s_status), "Stopping scan...");
            } else {
                snprintf(s_status, sizeof(s_status), "Scan already stopped");
            }
        } else {
            ui_screen_set(UI_SCREEN_WIFI_MENU);
        }
    }
    ui_render(canvas);
}

void ui_wifi_map_poll(gfx_canvas_t *canvas)
{
    bool render_needed = false;
    if (wifi_port_scan_is_running()) {
        wifi_port_scan_progress_t progress;
        if (wifi_port_scan_get_progress(&progress) &&
            (progress.scanned_count != s_last_scanned ||
             progress.current_port != s_last_port)) {
            s_last_scanned = progress.scanned_count;
            s_last_port = progress.current_port;
            snprintf(s_status, sizeof(s_status),
                     "%u/%u P:%u O:%u",
                     (unsigned)progress.scanned_count,
                     (unsigned)progress.total_count,
                     (unsigned)progress.current_port,
                     (unsigned)progress.open_count);
            render_needed = true;
        }
    }

    wifi_port_scan_result_t result;
    if (wifi_port_scan_receive_result(&result)) {
        sort_probes_for_display(&result);
        s_result = result;
        s_have_result = true;
        ui_focus_reset(UI_FOCUS_WIFI_MAP);
        if (result.finish == WIFI_PORT_SCAN_FINISH_CANCELLED) {
            snprintf(s_status, sizeof(s_status), "Scan cancelled");
        } else if (result.finish == WIFI_PORT_SCAN_FINISH_NETWORK_CHANGED) {
            snprintf(s_status, sizeof(s_status), "Network changed");
        } else if (result.finish == WIFI_PORT_SCAN_FINISH_RESOURCE_ERROR ||
                   result.error != ESP_OK) {
            snprintf(s_status, sizeof(s_status), "Socket resource error");
        } else {
            snprintf(s_status, sizeof(s_status), "Scan complete");
        }
        render_needed = true;
    }

    if (render_needed && canvas != NULL &&
        ui_screen_get() == UI_SCREEN_WIFI_MAP) {
        ui_render(canvas);
    }
}

void ui_wifi_map_draw(gfx_canvas_t *canvas, ui_cursor_t *cursor)
{
    gfx_canvas_fill(canvas, GFX_BLACK);
    gfx_canvas_draw_str(canvas, 10, 12, "Embedded MAP", UI_FONT, GFX_WHITE);
    gfx_canvas_draw_line(canvas, 0, 18, DISPLAY_WIDTH - 1, 18, GFX_WHITE);

    bool running = wifi_port_scan_is_running();
    if (s_have_result && !running &&
        s_result.finish == WIFI_PORT_SCAN_FINISH_COMPLETE &&
        s_result.error == ESP_OK) {
        char target[16];
        format_ipv4(s_result.target_ipv4, target);
        char heading[48];
        snprintf(heading, sizeof(heading), "%s  %s %u%%", target,
                 wifi_host_hint_name(s_result.classification.hint),
                 (unsigned)s_result.classification.confidence);
        uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_MAP);
        if (selected > s_result.probe_count) selected = 0U;
        update_heading_scroll(heading, selected == 0U);
        draw_heading_clipped(canvas, 37, heading, s_heading_scroll.offset_px);
        if (selected == 0U && !ui_wifi_has_focus()) {
            ui_cursor_set_target(cursor, 7, 24, 306, 17);
            ui_cursor_step(cursor);
            ui_cursor_draw(canvas, cursor, GFX_WHITE);
        }

        char summary[48];
        snprintf(summary, sizeof(summary), "%u open / %u scanned",
                 (unsigned)s_result.open_count,
                 (unsigned)s_result.scanned_count);
        gfx_canvas_draw_str(canvas, 10, 56, summary, UI_FONT, GFX_YELLOW);

        uint8_t selected_probe = selected > 0U
            ? (uint8_t)(selected - 1U) : 0U;
        uint8_t first = selected_probe >= MAP_RESULT_VISIBLE_ROWS
            ? (uint8_t)(selected_probe - MAP_RESULT_VISIBLE_ROWS + 1U) : 0U;
        if (s_result.probe_count > MAP_RESULT_VISIBLE_ROWS) {
            uint8_t last_start = (uint8_t)(s_result.probe_count -
                                           MAP_RESULT_VISIBLE_ROWS);
            if (first > last_start) first = last_start;
        }

        for (uint8_t row = 0U; row < MAP_RESULT_VISIBLE_ROWS; ++row) {
            uint8_t index = (uint8_t)(first + row);
            if (index >= s_result.probe_count) break;
            const wifi_probe_result_t *probe = &s_result.probes[index];
            char line[48];
            snprintf(line, sizeof(line), "%5u %-10s %-8s %ums",
                     (unsigned)probe->port,
                     wifi_port_scan_service_name(probe->service_hint),
                     wifi_port_scan_probe_state_name(probe->state),
                     (unsigned)probe->latency_ms);
            fit_text(line, sizeof(line), MAP_TEXT_MAX_WIDTH);
            int16_t baseline = (int16_t)(78 + row * 20);
            gfx_canvas_draw_str(canvas, 10, baseline, line, UI_FONT,
                                probe_color(probe->state));
            if (selected > 0U && index == selected_probe &&
                !ui_wifi_has_focus()) {
                ui_cursor_set_target(cursor, 7, baseline - 13, 304, 17);
                ui_cursor_step(cursor);
                ui_cursor_draw(canvas, cursor, GFX_WHITE);
            }
        }
        if (s_result.probe_count > MAP_RESULT_VISIBLE_ROWS) {
            int16_t track_y = 65;
            int16_t track_h = 78;
            int16_t thumb_h = (int16_t)(track_h * MAP_RESULT_VISIBLE_ROWS /
                                        s_result.probe_count);
            if (thumb_h < 8) thumb_h = 8;
            int16_t thumb_y = track_y;
            if (s_result.probe_count > 1U) {
                thumb_y += (int16_t)((track_h - thumb_h) * selected_probe /
                                     (s_result.probe_count - 1U));
            }
            gfx_canvas_draw_line(canvas, 316, track_y, 316,
                                 track_y + track_h, GFX_DARKGRAY);
            gfx_canvas_draw_line(canvas, 316, thumb_y, 316,
                                 thumb_y + thumb_h, GFX_WHITE);
        }
        const char *hint_left = "BACK";
        const char *hint_center = "SELECT";
        int16_t center_w = gfx_canvas_measure_text_width(UI_FONT, hint_center);
        gfx_canvas_draw_str(canvas, 10, 158, hint_left,
                            UI_FONT, GFX_DARKGRAY);
        gfx_canvas_draw_str(canvas, (int16_t)((320 - center_w) / 2), 158,
                            hint_center, UI_FONT, GFX_DARKGRAY);
        return;
    }

    const char *actions[MAP_ACTION_COUNT] = {
        "Quick: 12",
        "Common: 32",
        running ? "Stop scan" : "< BACK",
    };
    uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_MAP);
    if (selected >= MAP_ACTION_COUNT) selected = 0;

    for (uint8_t i = 0; i < MAP_ACTION_COUNT; ++i) {
        int16_t y = 39 + (int16_t)i * 21;
        uint16_t color = i == 2 && !running ? GFX_DARKGRAY : GFX_WHITE;
        gfx_canvas_draw_str(canvas, 10, y, actions[i], UI_FONT, color);
        if (i == selected && !ui_wifi_has_focus()) {
            int16_t width = gfx_canvas_measure_text_width(UI_FONT, actions[i]);
            ui_cursor_set_target(cursor, 7, y - 13, width + 6, 17);
            ui_cursor_step(cursor);
            ui_cursor_draw(canvas, cursor, GFX_WHITE);
        }
    }

    char target[48] = "Target: not connected";
    wifi_connection_snapshot_t connection = {0};
    if (wifi_worker_get_connection_snapshot(&connection) &&
        connection.info.connected) {
        char gateway[16];
        format_ipv4(connection.info.gateway, gateway);
        snprintf(target, sizeof(target), "Target: %s", gateway);
    }
    fit_text(target, sizeof(target), MAP_TEXT_MAX_WIDTH);
    gfx_canvas_draw_str(canvas, 10, 111, target, UI_FONT, GFX_DARKGRAY);

    if (s_have_result &&
        s_result.finish == WIFI_PORT_SCAN_FINISH_COMPLETE &&
        s_result.error == ESP_OK) {
        char counters[32];
        char ports[80];
        snprintf(counters, sizeof(counters), "O:%u C:%u F:%u E:%u",
                 (unsigned)s_result.open_count,
                 (unsigned)s_result.closed_count,
                 (unsigned)s_result.filtered_count,
                 (unsigned)s_result.probe_error_count);
        format_open_ports(ports, sizeof(ports));
        gfx_canvas_draw_str(canvas, 10, 132, counters, UI_FONT, GFX_YELLOW);
        gfx_canvas_draw_str(canvas, 10, 153, ports, UI_FONT,
                            s_result.open_count > 0 ? GFX_GREEN : GFX_DARKGRAY);
    } else {
        gfx_canvas_draw_str(canvas, 10, 132, s_status, UI_FONT,
                            running ? GFX_YELLOW : GFX_DARKGRAY);
    }
}

bool ui_wifi_map_pending(void)
{
    return wifi_port_scan_is_running();
}

bool ui_wifi_map_needs_tick(void)
{
    return s_have_result && !wifi_port_scan_is_running() &&
           ui_focus_get(UI_FOCUS_WIFI_MAP) == 0U &&
           s_heading_scroll.active;
}
