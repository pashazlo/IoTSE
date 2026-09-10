#include "ui_popup.h"
#include "ui_cursor.h"
#include "assets/ibm_vga_font.h"
#include <string.h>

#define POPUP_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)
/* IDs: 0 = close cross, 1 = first action, 2 = second action. */
static bool s_open, s_confirm_delete, s_error;
static unsigned s_focus;
static char s_name[64];
static char s_error_text[16];
static bool s_is_dir;
static ui_cursor_t s_cursor;

void ui_popup_open(const char *name, bool is_dir)
{
    if (!name) name = "";
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    s_is_dir = is_dir;
    s_confirm_delete = s_error = false;
    s_focus = 1; /* Rename: non-destructive initial focus. */
    s_open = true;
    ui_cursor_reset(&s_cursor);
}
void ui_popup_close(void)
{
    s_open = false;
    ui_cursor_reset(&s_cursor);
}
bool ui_popup_is_open(void) { return s_open; }
bool ui_popup_is_animating(void) { return s_open && s_cursor.animating; }
void ui_popup_show_error(const char *message)
{
    if (!message) message = "Failed";
    strncpy(s_error_text, message, sizeof(s_error_text) - 1);
    s_error_text[sizeof(s_error_text) - 1] = '\0';
    s_error = s_open = true;
    s_focus = 0;
    ui_cursor_reset(&s_cursor);
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
            ui_cursor_reset(&s_cursor);
            break;
        }
        ui_popup_close();
        return UI_POPUP_DELETE;
    default: break; /* A second hold never confirms or escapes to the browser. */
    }
    return UI_POPUP_NONE;
}

static void draw_action(gfx_canvas_t *c, int y, const char *label, unsigned id)
{
    bool selected = s_focus == id;
    gfx_canvas_fill_rect(c, 48, y, 224, 27, selected ? 0x2945 : 0x18C3);
    gfx_canvas_draw_str(c, 60, y + 18, label, POPUP_FONT,
                        selected ? 0xFFFF : 0xBDF7);
}
void ui_popup_draw(gfx_canvas_t *c)
{
    if (!c || !s_open) return;
    /* Solid panel with a small shadow; underlying browser is redrawn each frame. */
    gfx_canvas_fill_rect(c, 35, 23, 260, 130, 0x0000);
    gfx_canvas_fill_rect(c, 30, 18, 260, 130, 0x1082);
    gfx_canvas_draw_rect(c, 30, 18, 260, 130, 0x8410);
    const char *title = s_error ? "Error" : s_confirm_delete ? "Delete?" : s_is_dir ? "Folder" : "File";
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
        gfx_canvas_draw_str(c, 44, 88, s_error_text, POPUP_FONT, 0xFFFF);
        gfx_canvas_draw_str(c, 44, 120, "SELECT: close", POPUP_FONT, 0xBDF7);
    } else {
        draw_action(c, 69, s_confirm_delete ? "Cancel" : "Rename", 1);
        draw_action(c, 106, "Del", 2);
    }
    if (s_focus == 0) ui_cursor_set_target(&s_cursor, 263, 22, 20, 20);
    else ui_cursor_set_target(&s_cursor, 46, s_focus == 1 ? 67 : 104, 227, 30);
    ui_cursor_step(&s_cursor);
    ui_cursor_draw(c, &s_cursor, 0xFFFF);
}
