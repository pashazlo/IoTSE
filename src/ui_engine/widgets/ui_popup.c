#include "ui_popup.h"
#include "ui_cursor.h"
#include "wifi.h"
#include "assets/ibm_vga_font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#define POPUP_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)
#define POPUP_RADIUS 3
#define POPUP_BORDER 0x8410
#define POPUP_ITEM_BORDER 0x4208
#define POPUP_ITEM_ACTIVE 0xBDF7
#define POPUP_SCROLL_START_MS 1000U
#define POPUP_SCROLL_END_MS 1200U
#define POPUP_SCROLL_MS_PER_PIXEL 25U
typedef enum {
    POPUP_KIND_FILE = 0,
    POPUP_KIND_EDITOR,
    POPUP_KIND_EDITOR_EXIT,
    POPUP_KIND_ERROR,
    POPUP_KIND_WIFI,
    POPUP_KIND_WIFI_NETWORK,
    POPUP_KIND_WIFI_MAP_PORT,
    POPUP_KIND_MESSAGE,
} popup_kind_t;

/* ID 0 is always the close cross. */
static bool s_open, s_confirm_delete, s_error;
static unsigned s_focus;
static char s_name[64];
static char s_error_text[16];
static bool s_is_dir;
static bool s_wifi_saved, s_wifi_auto_connect, s_wifi_confirm_forget;
static popup_kind_t s_kind;
static ui_cursor_t s_cursor;
static char s_message_title[24];
static char s_message_line1[32];
static char s_message_line2[32];
static wifi_probe_result_t s_map_probe;
static wifi_banner_observation_t s_map_banner;
static bool s_map_has_banner;
static uint8_t s_map_scroll;
static uint8_t s_map_total_lines;

#define MAP_PORT_VISIBLE_LINES 5U
#define MAP_PORT_FIXED_LINES   7U
#define MAP_PORT_MAX_LINES     10U
#define MAP_PORT_TEXT_WIDTH    238

static size_t wifi_map_banner_chunk_length(const char *source)
{
    if (source == NULL || *source == '\0') return 0U;

    char candidate[40] = {0};
    size_t take = 0U;
    size_t last_space = 0U;
    while (source[take] != '\0' && take < sizeof(candidate) - 1U) {
        candidate[take] = source[take];
        candidate[take + 1U] = '\0';
        if (gfx_canvas_measure_text_width(POPUP_FONT, candidate) >
            MAP_PORT_TEXT_WIDTH) {
            break;
        }
        if (source[take] == ' ') last_space = take;
        ++take;
    }
    if (source[take] != '\0' && last_space > 12U) take = last_space;
    if (take == 0U) take = 1U;
    return take;
}

static uint8_t wifi_map_port_line_count(void)
{
    uint8_t count = MAP_PORT_FIXED_LINES;
    const char *source = s_map_has_banner ? s_map_banner.banner : NULL;
    for (uint8_t row = 0U;
         source != NULL && *source != '\0' && row < 3U;
         ++row) {
        size_t take = wifi_map_banner_chunk_length(source);
        if (take == 0U) break;
        ++count;
        source += take;
        while (*source == ' ') ++source;
    }
    return count;
}

typedef struct {
    char text[64];
    popup_kind_t kind;
    unsigned focus;
    int16_t view_width;
    int16_t offset_px;
    uint32_t elapsed_ms;
    TickType_t last_tick;
    bool active;
} popup_marquee_t;

static popup_marquee_t s_marquee;

static void reset_popup_motion(void)
{
    ui_cursor_reset(&s_cursor);
    memset(&s_marquee, 0, sizeof(s_marquee));
}

void ui_popup_open(const char *name, bool is_dir)
{
    if (!name) name = "";
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    s_is_dir = is_dir;
    s_kind = POPUP_KIND_FILE;
    s_confirm_delete = s_error = false;
    s_focus = 1; /* Rename: non-destructive initial focus. */
    s_open = true;
    reset_popup_motion();
}
void ui_popup_open_editor(const char *filename)
{
    if (!filename) filename = "";
    strncpy(s_name, filename, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    s_kind = POPUP_KIND_EDITOR;
    s_confirm_delete = s_error = false;
    s_focus = 1;
    s_open = true;
    reset_popup_motion();
}
void ui_popup_open_editor_exit(const char *filename)
{
    if (!filename) filename = "";
    strncpy(s_name, filename, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    s_kind = POPUP_KIND_EDITOR_EXIT;
    s_confirm_delete = s_error = false;
    s_focus = 1; /* Save is the safe default. */
    s_open = true;
    reset_popup_motion();
}
void ui_popup_open_wifi(void)
{
    s_kind = POPUP_KIND_WIFI;
    s_confirm_delete = s_error = false;
    s_focus = 1;
    s_open = true;
    reset_popup_motion();
}
void ui_popup_open_wifi_network(
    const char *ssid,
    bool saved,
    bool auto_connect
)
{
    if (!ssid) ssid = "";
    strncpy(s_name, ssid, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    s_kind = POPUP_KIND_WIFI_NETWORK;
    s_wifi_saved = saved;
    s_wifi_auto_connect = auto_connect;
    s_wifi_confirm_forget = false;
    s_confirm_delete = s_error = false;
    s_focus = 1;
    s_open = true;
    reset_popup_motion();
}
void ui_popup_open_wifi_map_port(
    const wifi_probe_result_t *probe,
    const wifi_banner_observation_t *banner
)
{
    if (probe == NULL) return;
    s_map_probe = *probe;
    s_map_has_banner = banner != NULL && banner->banner_length > 0U;
    if (s_map_has_banner) s_map_banner = *banner;
    else memset(&s_map_banner, 0, sizeof(s_map_banner));
    s_map_scroll = 0U;
    s_map_total_lines = wifi_map_port_line_count();
    s_kind = POPUP_KIND_WIFI_MAP_PORT;
    s_error = false;
    s_focus = 0;
    s_open = true;
    reset_popup_motion();
}
void ui_popup_show_message(
    const char *title,
    const char *line1,
    const char *line2
)
{
    snprintf(s_message_title, sizeof(s_message_title), "%s",
             title != NULL ? title : "Info");
    snprintf(s_message_line1, sizeof(s_message_line1), "%s",
             line1 != NULL ? line1 : "");
    snprintf(s_message_line2, sizeof(s_message_line2), "%s",
             line2 != NULL ? line2 : "");
    s_kind = POPUP_KIND_MESSAGE;
    s_error = false;
    s_focus = 0;
    s_open = true;
    reset_popup_motion();
}
void ui_popup_close(void)
{
    s_open = false;
    reset_popup_motion();
}
bool ui_popup_is_open(void) { return s_open; }
bool ui_popup_is_animating(void)
{
    bool marquee_visible = s_marquee.active && s_marquee.kind == s_kind &&
                            s_marquee.focus == s_focus && s_focus != 0;
    return s_open && (s_cursor.animating || marquee_visible);
}
void ui_popup_show_error(const char *message)
{
    if (!message) message = "Failed";
    strncpy(s_error_text, message, sizeof(s_error_text) - 1);
    s_error_text[sizeof(s_error_text) - 1] = '\0';
    s_error = s_open = true;
    s_kind = POPUP_KIND_ERROR;
    s_focus = 0;
    reset_popup_motion();
}
ui_popup_result_t ui_popup_handle_event(ui_event_t evt)
{
    if (!s_open) return UI_POPUP_NONE;
    if (evt == UI_EVT_LEFT) {
        ui_popup_close();
        return UI_POPUP_CANCEL;
    }
    if (s_error) {
        if (evt == UI_EVT_SELECT) {
            ui_popup_close();
            return UI_POPUP_CANCEL;
        }
        return UI_POPUP_NONE;
    }
    if (s_kind == POPUP_KIND_MESSAGE) {
        if (evt == UI_EVT_SELECT || evt == UI_EVT_RIGHT) {
            ui_popup_close();
            return UI_POPUP_CANCEL;
        }
        return UI_POPUP_NONE;
    }
    if (s_kind == POPUP_KIND_WIFI_MAP_PORT) {
        uint8_t max_scroll = s_map_total_lines > MAP_PORT_VISIBLE_LINES
            ? (uint8_t)(s_map_total_lines - MAP_PORT_VISIBLE_LINES) : 0U;
        if (evt == UI_EVT_UP && s_map_scroll > 0U) {
            --s_map_scroll;
        } else if (evt == UI_EVT_DOWN && s_map_scroll < max_scroll) {
            ++s_map_scroll;
        } else if (evt == UI_EVT_SELECT) {
            ui_popup_close();
            return UI_POPUP_CANCEL;
        }
        return UI_POPUP_NONE;
    }

    if (s_kind == POPUP_KIND_EDITOR) {
        switch (evt) {
        case UI_EVT_UP: s_focus = (s_focus + 4) % 5; break;
        case UI_EVT_DOWN: s_focus = (s_focus + 1) % 5; break;
        case UI_EVT_RIGHT: s_focus = 0; break;
        case UI_EVT_SELECT: {
            static const ui_popup_result_t results[] = {
                UI_POPUP_CANCEL,
                UI_POPUP_EDITOR_NEW_LINE,
                UI_POPUP_EDITOR_BACKSPACE,
                UI_POPUP_EDITOR_SAVE,
                UI_POPUP_EDITOR_SAVE_EXIT,
            };
            ui_popup_result_t result = results[s_focus];
            ui_popup_close();
            return result;
        }
        default: break;
        }
        return UI_POPUP_NONE;
    }
    if (s_kind == POPUP_KIND_EDITOR_EXIT) {
        switch (evt) {
        case UI_EVT_UP: s_focus = (s_focus + 3) % 4; break;
        case UI_EVT_DOWN: s_focus = (s_focus + 1) % 4; break;
        case UI_EVT_RIGHT: s_focus = 0; break;
        case UI_EVT_SELECT: {
            static const ui_popup_result_t results[] = {
                UI_POPUP_CANCEL,
                UI_POPUP_EDITOR_EXIT_SAVE,
                UI_POPUP_EDITOR_DISCARD,
                UI_POPUP_CANCEL,
            };
            ui_popup_result_t result = results[s_focus];
            ui_popup_close();
            return result;
        }
        default: break;
        }
        return UI_POPUP_NONE;
    }
    if (s_kind == POPUP_KIND_WIFI) {
        switch (evt) {
        case UI_EVT_UP:
        case UI_EVT_DOWN: s_focus = s_focus == 0 ? 1 : 0; break;
        case UI_EVT_RIGHT: s_focus = 0; break;
        case UI_EVT_SELECT: {
            ui_popup_result_t result = s_focus == 0
                ? UI_POPUP_CANCEL : UI_POPUP_WIFI_DISCONNECT;
            ui_popup_close();
            return result;
        }
        default: break;
        }
        return UI_POPUP_NONE;
    }
    if (s_kind == POPUP_KIND_WIFI_NETWORK) {
        if (s_wifi_confirm_forget) {
            switch (evt) {
            case UI_EVT_UP:
            case UI_EVT_DOWN: s_focus = s_focus == 1 ? 2 : 1; break;
            case UI_EVT_RIGHT: s_focus = 0; break;
            case UI_EVT_SELECT:
                if (s_focus == 2) {
                    ui_popup_close();
                    return UI_POPUP_WIFI_FORGET;
                }
                if (s_focus == 0) {
                    ui_popup_close();
                    return UI_POPUP_CANCEL;
                }
                s_wifi_confirm_forget = false;
                s_focus = 2;
                reset_popup_motion();
                break;
            default: break;
            }
            return UI_POPUP_NONE;
        }

        switch (evt) {
        case UI_EVT_UP: s_focus = s_focus <= 1 ? 3 : s_focus - 1; break;
        case UI_EVT_DOWN: s_focus = s_focus >= 3 ? 1 : s_focus + 1; break;
        case UI_EVT_RIGHT: s_focus = 0; break;
        case UI_EVT_SELECT:
            if (s_focus == 0) {
                ui_popup_close();
                return UI_POPUP_CANCEL;
            }
            if (s_focus == 2 && s_wifi_saved) {
                s_wifi_confirm_forget = true;
                s_focus = 1; /* Destructive confirmation starts on Cancel. */
                reset_popup_motion();
                return UI_POPUP_NONE;
            }
            {
                static const ui_popup_result_t results[] = {
                    UI_POPUP_NONE,
                    UI_POPUP_WIFI_AUTO_TOGGLE,
                    UI_POPUP_WIFI_FORGET,
                    UI_POPUP_WIFI_WEB,
                };
                ui_popup_result_t result = results[s_focus];
                ui_popup_close();
                return result;
            }
        default: break;
        }
        return UI_POPUP_NONE;
    }
    switch (evt) {
    case UI_EVT_UP: s_focus = (s_focus + 2) % 3; break;
    case UI_EVT_DOWN: s_focus = (s_focus + 1) % 3; break;
    case UI_EVT_RIGHT: s_focus = 0; break;
    case UI_EVT_SELECT:
        if (s_focus == 0 || (s_confirm_delete && s_focus == 1)) {
            ui_popup_close();
            return UI_POPUP_CANCEL;
        }
        if (!s_confirm_delete && s_focus == 1) {
            ui_popup_close();
            return UI_POPUP_RENAME;
        }
        if (!s_confirm_delete) {
            s_confirm_delete = true;
            s_focus = 1; /* Cancel is selected when confirmation opens. */
            reset_popup_motion();
            break;
        }
        ui_popup_close();
        return UI_POPUP_DELETE;
    default: break; /* A second hold never confirms or escapes to the browser. */
    }
    return UI_POPUP_NONE;
}

static void draw_clipped_text(
    gfx_canvas_t *canvas,
    int16_t left,
    int16_t right,
    int16_t baseline,
    const char *text,
    int16_t offset_px,
    uint16_t color
)
{
    if (canvas == NULL || text == NULL || left >= right) return;
    int16_t pen_x = (int16_t)(left - offset_px);
    const unsigned char *cursor = (const unsigned char *)text;

    for (; *cursor != '\0'; ++cursor) {
        if (*cursor < POPUP_FONT->first || *cursor > POPUP_FONT->last) {
            continue;
        }
        const gfx_glyph_t *glyph =
            &POPUP_FONT->glyphs[*cursor - POPUP_FONT->first];
        for (int yy = 0; yy < glyph->height; ++yy) {
            for (int xx = 0; xx < glyph->width; ++xx) {
                int16_t x = (int16_t)(pen_x + glyph->xOffset + xx);
                if (x < left || x >= right) continue;
                size_t bit = (size_t)yy * glyph->width + xx;
                if (POPUP_FONT->bitmap[glyph->bitmapOffset + bit / 8U] &
                    (0x80U >> (bit % 8U))) {
                    gfx_canvas_draw_pixel(
                        canvas,
                        x,
                        (int16_t)(baseline + glyph->yOffset + yy),
                        color
                    );
                }
            }
        }
        pen_x = (int16_t)(pen_x + glyph->xAdvance);
        if (pen_x >= right) break;
    }
}

static void update_marquee(const char *text, unsigned focus, int16_t view_width)
{
    TickType_t now = xTaskGetTickCount();
    bool changed = s_marquee.kind != s_kind ||
                   s_marquee.focus != focus ||
                   s_marquee.view_width != view_width ||
                   strcmp(s_marquee.text, text) != 0;
    if (changed) {
        snprintf(s_marquee.text, sizeof(s_marquee.text), "%s", text);
        s_marquee.kind = s_kind;
        s_marquee.focus = focus;
        s_marquee.view_width = view_width;
        s_marquee.offset_px = 0;
        s_marquee.elapsed_ms = 0;
    }

    int16_t distance = (int16_t)(
        gfx_canvas_measure_text_width(POPUP_FONT, text) - view_width
    );
    s_marquee.active = distance > 0;
    if (s_marquee.active && !changed) {
        uint64_t delta_ms =
            (uint64_t)(TickType_t)(now - s_marquee.last_tick) * 1000U /
            configTICK_RATE_HZ;
        uint32_t move_ms = (uint32_t)distance * POPUP_SCROLL_MS_PER_PIXEL;
        uint32_t cycle_ms = POPUP_SCROLL_START_MS + move_ms +
                            POPUP_SCROLL_END_MS;
        s_marquee.elapsed_ms =
            (s_marquee.elapsed_ms + (uint32_t)delta_ms) % cycle_ms;
        if (s_marquee.elapsed_ms <= POPUP_SCROLL_START_MS) {
            s_marquee.offset_px = 0;
        } else if (s_marquee.elapsed_ms >= POPUP_SCROLL_START_MS + move_ms) {
            s_marquee.offset_px = distance;
        } else {
            s_marquee.offset_px = (int16_t)(
                (s_marquee.elapsed_ms - POPUP_SCROLL_START_MS) /
                POPUP_SCROLL_MS_PER_PIXEL
            );
        }
    } else if (!s_marquee.active) {
        s_marquee.offset_px = 0;
        s_marquee.elapsed_ms = 0;
    }
    s_marquee.last_tick = now;
}

static void draw_action_text(
    gfx_canvas_t *canvas,
    int16_t left,
    int16_t right,
    int16_t baseline,
    const char *label,
    unsigned id,
    uint16_t color
)
{
    int16_t offset = 0;
    if (s_focus == id) {
        update_marquee(label, id, (int16_t)(right - left));
        offset = s_marquee.offset_px;
    }
    draw_clipped_text(canvas, left, right, baseline, label, offset, color);
}

static void draw_action(gfx_canvas_t *c, int y, const char *label, unsigned id)
{
    bool selected = s_focus == id;
    gfx_canvas_draw_round_rect(c, 48, y, 224, 27, POPUP_RADIUS,
                               selected ? POPUP_ITEM_ACTIVE
                                        : POPUP_ITEM_BORDER);
    draw_action_text(c, 60, 260, y + 18, label, id,
                     selected ? 0xFFFF : 0xBDF7);
}

static void fill_rounded_panel(
    gfx_canvas_t *c,
    int16_t x,
    int16_t y,
    int16_t w,
    int16_t h
)
{
    gfx_canvas_fill_round_rect(c, x, y, w, h, POPUP_RADIUS, GFX_BLACK);
    gfx_canvas_draw_round_rect(c, x, y, w, h, POPUP_RADIUS, POPUP_BORDER);
}

static void fit_text(char *out, size_t out_size, const char *text, int16_t max_width)
{
    snprintf(out, out_size, "%s", text != NULL ? text : "");
    if (gfx_canvas_measure_text_width(POPUP_FONT, out) <= max_width) return;

    size_t length = strlen(out);
    while (length > 3U &&
           gfx_canvas_measure_text_width(POPUP_FONT, out) > max_width) {
        --length;
        out[length] = '\0';
    }
    if (length >= 3U) {
        out[length - 3U] = '.';
        out[length - 2U] = '.';
        out[length - 1U] = '.';
    }
}

static void draw_network_action(
    gfx_canvas_t *c,
    int16_t y,
    const char *label,
    unsigned id,
    bool enabled
)
{
    uint16_t color = enabled ? 0xBDF7 : 0x4208;
    gfx_canvas_draw_round_rect(c, 32, y, 256, 24, POPUP_RADIUS, color);
    draw_action_text(c, 43, 277, y + 16, label, id,
                     s_focus == id && enabled ? 0xFFFF : color);
}

static void draw_wifi_network_popup(gfx_canvas_t *c)
{
    fill_rounded_panel(c, 20, 17, 280, 136);
    gfx_canvas_draw_str(c, 34, 36,
                        s_wifi_confirm_forget ? "Forget network?" : "Wi-Fi network",
                        POPUP_FONT, 0xFFFF);
    gfx_canvas_draw_line(c, 277, 24, 287, 34, 0xFFFF);
    gfx_canvas_draw_line(c, 287, 24, 277, 34, 0xFFFF);

    char ssid[WIFI_SSID_MAX_LEN + 1];
    fit_text(ssid, sizeof(ssid), s_name, 246);
    gfx_canvas_draw_str(c, 34, 55, ssid, POPUP_FONT, 0x7BEF);

    if (s_wifi_confirm_forget) {
        draw_network_action(c, 73, "Cancel", 1, true);
        draw_network_action(c, 107, "Forget", 2, true);
        if (s_focus == 0) ui_cursor_set_target(&s_cursor, 272, 20, 20, 20);
        else ui_cursor_set_target(&s_cursor, 29,
                                  s_focus == 1 ? 70 : 104, 262, 29);
    } else {
        char auto_label[16];
        snprintf(auto_label, sizeof(auto_label), "[%c] Auto",
                 s_wifi_saved && s_wifi_auto_connect ? 'x' : ' ');
        draw_network_action(c, 61, auto_label, 1, s_wifi_saved);
        draw_network_action(c, 89, "Forget network", 2, s_wifi_saved);
        draw_network_action(c, 117, "Web UI (soon)", 3, true);
        if (s_focus == 0) ui_cursor_set_target(&s_cursor, 272, 20, 20, 20);
        else ui_cursor_set_target(&s_cursor, 29,
                                  58 + (int16_t)(s_focus - 1U) * 28,
                                  262, 29);
    }
    ui_cursor_step(&s_cursor);
    ui_cursor_draw(c, &s_cursor, 0xFFFF);
}

static void draw_message_popup(gfx_canvas_t *c)
{
    fill_rounded_panel(c, 30, 28, 260, 114);
    draw_clipped_text(c, 44, 260, 48, s_message_title, 0, 0xFFFF);
    gfx_canvas_draw_line(c, 268, 35, 278, 45, 0xFFFF);
    gfx_canvas_draw_line(c, 278, 35, 268, 45, 0xFFFF);
    draw_clipped_text(c, 44, 278, 76, s_message_line1, 0, 0xC618);
    draw_clipped_text(c, 44, 278, 98, s_message_line2, 0, 0xC618);
    gfx_canvas_draw_str(c, 44, 126, "SELECT: close", POPUP_FONT, 0x7BEF);
    ui_cursor_set_target(&s_cursor, 263, 31, 20, 20);
    ui_cursor_step(&s_cursor);
    ui_cursor_draw(c, &s_cursor, 0xFFFF);
}

static void draw_wifi_map_port_popup(gfx_canvas_t *c)
{
    fill_rounded_panel(c, 20, 5, 280, 160);
    char title[24];
    snprintf(title, sizeof(title), "Port %u", (unsigned)s_map_probe.port);
    gfx_canvas_draw_str(c, 34, 25, title, POPUP_FONT, 0xFFFF);
    gfx_canvas_draw_line(c, 277, 12, 287, 22, 0xFFFF);
    gfx_canvas_draw_line(c, 287, 12, 277, 22, 0xFFFF);
    gfx_canvas_draw_line(c, 32, 33, 288, 33, 0x39E7);

    char lines[MAP_PORT_MAX_LINES][40] = {{0}};
    snprintf(lines[0], sizeof(lines[0]), "State: %s",
             wifi_port_scan_probe_state_name(s_map_probe.state));
    snprintf(lines[1], sizeof(lines[1]), "Service: %s",
             wifi_port_scan_service_name(s_map_probe.service_hint));
    snprintf(lines[2], sizeof(lines[2]), "RTT: %u ms",
             (unsigned)s_map_probe.latency_ms);
    snprintf(lines[3], sizeof(lines[3]), "Errno: %ld",
             (long)s_map_probe.socket_error);
    snprintf(lines[4], sizeof(lines[4]), "Evidence:");
    snprintf(lines[5], sizeof(lines[5]), "%s",
             s_map_has_banner ? (s_map_banner.source == WIFI_BANNER_PASSIVE
                 ? "passive banner" : "active response") : "port hint only");
    snprintf(lines[6], sizeof(lines[6]), "Banner:%s",
             s_map_has_banner ? "" : " none");
    if (s_map_has_banner) {
        const char *source = s_map_banner.banner;
        for (unsigned row = 0U; row < 3U && *source != '\0'; ++row) {
            size_t take = wifi_map_banner_chunk_length(source);
            if (take == 0U) break;
            memcpy(lines[7U + row], source, take);
            lines[7U + row][take] = '\0';
            source += take;
            while (*source == ' ') ++source;
        }
    }

    const uint8_t total_lines = s_map_total_lines;
    const uint8_t visible_lines = MAP_PORT_VISIBLE_LINES;
    uint8_t max_scroll = total_lines > visible_lines
        ? (uint8_t)(total_lines - visible_lines) : 0U;
    if (s_map_scroll > max_scroll) s_map_scroll = max_scroll;
    for (uint8_t row = 0U; row < visible_lines; ++row) {
        uint8_t index = (uint8_t)(s_map_scroll + row);
        if (index >= total_lines) break;
        uint16_t color = index == 0U && s_map_probe.state == WIFI_PROBE_OPEN
            ? GFX_GREEN : 0xC618;
        draw_clipped_text(c, 34, 278, (int16_t)(50 + row * 17),
                          lines[index], 0, color);
    }
    if (max_scroll > 0U) {
        const int16_t track_y = 41;
        const int16_t track_h = 82;
        int16_t thumb_h = (int16_t)(track_h * visible_lines / total_lines);
        if (thumb_h < 10) thumb_h = 10;
        int16_t thumb_y = (int16_t)(track_y +
            (track_h - thumb_h) * s_map_scroll / max_scroll);
        gfx_canvas_draw_line(c, 287, track_y, 287,
                             track_y + track_h, 0x39E7);
        gfx_canvas_draw_line(c, 287, thumb_y, 287,
                             thumb_y + thumb_h, 0xFFFF);
        gfx_canvas_draw_line(c, 288, thumb_y, 288,
                             thumb_y + thumb_h, 0xFFFF);
    }
    gfx_canvas_draw_line(c, 32, 132, 288, 132, 0x39E7);
    const char *scroll_hint = "UP/DN";
    int16_t hint_w = gfx_canvas_measure_text_width(POPUP_FONT, scroll_hint);
    gfx_canvas_draw_str(c, (int16_t)(160 - hint_w / 2), 151,
                        scroll_hint, POPUP_FONT, 0x7BEF);
    ui_cursor_set_target(&s_cursor, 272, 8, 20, 20);
    ui_cursor_step(&s_cursor);
    ui_cursor_draw(c, &s_cursor, 0xFFFF);
}

static void draw_editor_popup(gfx_canvas_t *c)
{
    static const char *actions[] = {
        "New line",
        "Backspace",
        "Save",
        "Save & Exit",
    };

    fill_rounded_panel(c, 30, 3, 260, 157);
    gfx_canvas_draw_str(c, 44, 23, "Editor", POPUP_FONT, 0xFFFF);
    gfx_canvas_draw_line(c, 268, 12, 278, 22, 0xFFFF);
    gfx_canvas_draw_line(c, 278, 12, 268, 22, 0xFFFF);

    char label[15];
    strncpy(label, s_name, sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    if (strlen(s_name) > 14) {
        label[11] = '.';
        label[12] = '.';
        label[13] = '.';
    }
    gfx_canvas_draw_str(c, 44, 42, label, POPUP_FONT, 0xBDF7);

    for (unsigned i = 0; i < 4; i++) {
        int y = 47 + (int)i * 27;
        bool selected = s_focus == i + 1;
        gfx_canvas_draw_round_rect(c, 48, y, 224, 23, POPUP_RADIUS,
                                   selected ? POPUP_ITEM_ACTIVE
                                            : POPUP_ITEM_BORDER);
        draw_action_text(c, 60, 260, y + 16, actions[i], i + 1,
                         selected ? 0xFFFF : 0xBDF7);
    }

    if (s_focus == 0) {
        ui_cursor_set_target(&s_cursor, 263, 7, 20, 20);
    } else {
        ui_cursor_set_target(
            &s_cursor,
            46,
            45 + (int)(s_focus - 1) * 27,
            227,
            26
        );
    }
    ui_cursor_step(&s_cursor);
    ui_cursor_draw(c, &s_cursor, 0xFFFF);
}

static void draw_editor_exit_popup(gfx_canvas_t *c)
{
    static const char *actions[] = {"Save", "Discard", "Cancel"};

    fill_rounded_panel(c, 30, 18, 260, 130);
    gfx_canvas_draw_str(c, 44, 38, "Unsaved", POPUP_FONT, 0xFFFF);
    gfx_canvas_draw_line(c, 268, 27, 278, 37, 0xFFFF);
    gfx_canvas_draw_line(c, 278, 27, 268, 37, 0xFFFF);

    for (unsigned i = 0; i < 3; i++) {
        int y = 50 + (int)i * 30;
        bool selected = s_focus == i + 1;
        gfx_canvas_draw_round_rect(c, 48, y, 224, 25, POPUP_RADIUS,
                                   selected ? POPUP_ITEM_ACTIVE
                                            : POPUP_ITEM_BORDER);
        draw_action_text(c, 60, 260, y + 17, actions[i], i + 1,
                         selected ? 0xFFFF : 0xBDF7);
    }

    if (s_focus == 0) {
        ui_cursor_set_target(&s_cursor, 263, 22, 20, 20);
    } else {
        ui_cursor_set_target(&s_cursor, 46, 48 + (int)(s_focus - 1) * 30, 227, 28);
    }
    ui_cursor_step(&s_cursor);
    ui_cursor_draw(c, &s_cursor, 0xFFFF);
}
void ui_popup_draw(gfx_canvas_t *c)
{
    if (!c || !s_open) return;
    gfx_canvas_dim(c);
    if (s_kind == POPUP_KIND_WIFI_NETWORK) {
        draw_wifi_network_popup(c);
        return;
    }
    if (s_kind == POPUP_KIND_MESSAGE) {
        draw_message_popup(c);
        return;
    }
    if (s_kind == POPUP_KIND_WIFI_MAP_PORT) {
        draw_wifi_map_port_popup(c);
        return;
    }
    if (s_kind == POPUP_KIND_EDITOR) {
        draw_editor_popup(c);
        return;
    }
    if (s_kind == POPUP_KIND_EDITOR_EXIT) {
        draw_editor_exit_popup(c);
        return;
    }
    if (s_kind == POPUP_KIND_WIFI) {
        fill_rounded_panel(c, 30, 33, 260, 95);
        gfx_canvas_draw_str(c, 44, 54, "Wi-Fi actions", POPUP_FONT, 0xFFFF);
        gfx_canvas_draw_line(c, 268, 42, 278, 52, 0xFFFF);
        gfx_canvas_draw_line(c, 278, 42, 268, 52, 0xFFFF);
        draw_action(c, 72, "Disconnect", 1);
        if (s_focus == 0) ui_cursor_set_target(&s_cursor, 263, 37, 20, 20);
        else ui_cursor_set_target(&s_cursor, 46, 70, 227, 30);
        ui_cursor_step(&s_cursor);
        ui_cursor_draw(c, &s_cursor, 0xFFFF);
        return;
    }
    fill_rounded_panel(c, 30, 18, 260, 130);
    const char *title = s_error ? "Error" :
        s_confirm_delete ? (s_is_dir ? "Delete dir?" : "Delete?") :
        s_is_dir ? "Folder" : "File";
    gfx_canvas_draw_str(c, 44, 38, title, POPUP_FONT, 0xFFFF);
    /* Actual cross, independent of font glyph support. */
    gfx_canvas_draw_line(c, 268, 27, 278, 37, 0xFFFF);
    gfx_canvas_draw_line(c, 278, 27, 268, 37, 0xFFFF);
    char label[15];
    strncpy(label, s_name, sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    if (strlen(s_name) > 14) { label[11] = '.'; label[12] = '.'; label[13] = '.'; }
    gfx_canvas_draw_str(c, 44, 59, label, POPUP_FONT, 0xBDF7);
    if (s_error) {
        draw_clipped_text(c, 44, 278, 88, s_error_text, 0, 0xFFFF);
        gfx_canvas_draw_str(c, 44, 120, "SELECT: close", POPUP_FONT, 0xBDF7);
    } else {
        draw_action(c, 69, s_confirm_delete ? "Cancel" : "Rename", 1);
        draw_action(
            c,
            106,
            s_confirm_delete && s_is_dir ? "Del contents" : "Del",
            2
        );
    }
    if (s_focus == 0) ui_cursor_set_target(&s_cursor, 263, 22, 20, 20);
    else ui_cursor_set_target(&s_cursor, 46, s_focus == 1 ? 67 : 104, 227, 30);
    ui_cursor_step(&s_cursor);
    ui_cursor_draw(c, &s_cursor, 0xFFFF);
}
