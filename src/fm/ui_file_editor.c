#include "ui_file_editor.h"

#include "fm_text_edit.h"
#include "fm_worker.h"
#include "ui_keyboard.h"
#include "display.h"
#include "assets/ibm_vga_font.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)

/* Keep the same header geometry as the menu and file-browser screens. */
#define EDITOR_HEADER_BASELINE       12
#define EDITOR_HEADER_DIVIDER_Y      18
#define EDITOR_BODY_BASELINE         32

#define EDITOR_LINE_NUMBER_X         2
#define EDITOR_LINE_NUMBER_MIN_DIGITS 3
#define EDITOR_LINE_NUMBER_GAP       4
#define EDITOR_RIGHT_MARGIN          2

#define EDITOR_TEXT_ASCENT           11
#define EDITOR_TEXT_DESCENT          3
#define EDITOR_LINE_HEIGHT           16
#define EDITOR_CURSOR_BLINK_MS       500
#define EDITOR_VISIBLE_TEXT_MAX      32
#define EDITOR_TITLE_MAX             64

_Static_assert(
    UI_KEYBOARD_MAX_TEXT_LEN == FM_EDIT_MAX_LINE_LEN,
    "Keyboard and editor line limits must match"
);

typedef struct {
    uint16_t cursor_line;
    uint16_t cursor_column;
    uint16_t scroll_y;
    uint16_t scroll_x;
    bool dirty;
    bool io_pending;
    bool exit_after_save;
} ui_editor_state_t;

static ui_editor_state_t s_editor;
static TickType_t s_cursor_epoch;

static uint16_t editor_glyph_advance(unsigned char ch)
{
    if (ch < UI_FONT->first || ch > UI_FONT->last) {
        ch = ' ';
    }

    return UI_FONT->glyphs[ch - UI_FONT->first].xAdvance;
}

static uint16_t editor_cell_width(void)
{
    return editor_glyph_advance(' ');
}

static uint16_t editor_line_number_digits(void)
{
    uint32_t highest_line = fm_text_edit_line_count();
    highest_line = highest_line > 0 ? highest_line - 1 : 0;
    uint16_t digits = 1;

    while (highest_line >= 10U) {
        highest_line /= 10U;
        digits++;
    }

    return digits < EDITOR_LINE_NUMBER_MIN_DIGITS
        ? EDITOR_LINE_NUMBER_MIN_DIGITS
        : digits;
}

static int16_t editor_text_x(void)
{
    uint16_t digits = editor_line_number_digits();
    return EDITOR_LINE_NUMBER_X +
           (int16_t)(digits * editor_glyph_advance('0')) +
           EDITOR_LINE_NUMBER_GAP;
}

static uint16_t editor_visible_columns(void)
{
    int16_t available_px = DISPLAY_WIDTH - editor_text_x() - EDITOR_RIGHT_MARGIN;
    uint16_t cell_width = editor_cell_width();

    if (available_px <= 0 || cell_width == 0) {
        return 1;
    }

    uint16_t columns = (uint16_t)(available_px / cell_width);
    return columns > 0 ? columns : 1;
}

static uint16_t editor_visible_lines(void)
{
    int16_t available_px = DISPLAY_HEIGHT -
                           EDITOR_BODY_BASELINE -
                           EDITOR_TEXT_DESCENT;

    if (available_px < 0) {
        return 1;
    }

    return (uint16_t)(available_px / EDITOR_LINE_HEIGHT) + 1;
}

static int16_t editor_columns_pixel_width(
    const char *line,
    uint16_t first_column,
    uint16_t last_column
)
{
    if (last_column <= first_column) {
        return 0;
    }

    size_t line_len = line != NULL
        ? strnlen(line, FM_EDIT_MAX_LINE_LEN)
        : 0;
    int32_t width = 0;

    for (uint16_t column = first_column; column < last_column; column++) {
        unsigned char ch = column < line_len
            ? (unsigned char)line[column]
            : (unsigned char)' ';
        width += editor_glyph_advance(ch);
    }

    return width > INT16_MAX ? INT16_MAX : (int16_t)width;
}

static void editor_make_title(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }

    const char *path = fm_text_edit_get_filepath();
    const char *name = path != NULL ? strrchr(path, '/') : NULL;
    const char *backslash = path != NULL ? strrchr(path, '\\') : NULL;

    if (backslash != NULL && (name == NULL || backslash > name)) {
        name = backslash;
    }
    name = name != NULL ? name + 1 : path;

    if (name == NULL || name[0] == '\0') {
        name = "Editor";
    }

    snprintf(out, out_size, "%s", name);

    const int16_t status_space = s_editor.io_pending
        ? 100
        : s_editor.dirty ? 36 : 20;
    const int16_t max_width = DISPLAY_WIDTH - status_space;
    size_t len = strlen(out);
    while (len > 3 && gfx_canvas_measure_text_width(UI_FONT, out) > max_width) {
        len--;
        out[len] = '\0';
    }

    if (strcmp(out, name) != 0 && len >= 3) {
        memcpy(out + len - 3, "...", 3);
    }
}

static void adjust_scroll(void)
{
    uint16_t visible_lines = editor_visible_lines();
    uint16_t visible_columns = editor_visible_columns();

    if (s_editor.cursor_line < s_editor.scroll_y) {
        s_editor.scroll_y = s_editor.cursor_line;
    } else if (s_editor.cursor_line >= s_editor.scroll_y + visible_lines) {
        s_editor.scroll_y = s_editor.cursor_line - visible_lines + 1;
    }

    if (s_editor.cursor_column < s_editor.scroll_x) {
        s_editor.scroll_x = s_editor.cursor_column;
    } else if (s_editor.cursor_column > s_editor.scroll_x + visible_columns) {
        s_editor.scroll_x = s_editor.cursor_column - visible_columns;
    }
}

static void reset_cursor_blink(void)
{
    s_cursor_epoch = xTaskGetTickCount();
}

static bool editor_start_save(bool exit_after_save)
{
    if (s_editor.io_pending) {
        return false;
    }
    if (fm_worker_send_save_file(fm_text_edit_get_filepath()) != ESP_OK) {
        ui_popup_show_error("Worker busy");
        return false;
    }
    s_editor.io_pending = true;
    s_editor.exit_after_save = exit_after_save;
    return true;
}

static void editor_open_exit_prompt(void)
{
    char title[EDITOR_TITLE_MAX];
    editor_make_title(title, sizeof(title));
    ui_popup_open_editor_exit(title);
}

void ui_file_editor_init(void)
{
    memset(&s_editor, 0, sizeof(s_editor));
    fm_text_edit_init();
    reset_cursor_blink();
}

void ui_file_editor_open_loaded(void)
{
    memset(&s_editor, 0, sizeof(s_editor));
    reset_cursor_blink();
}

void ui_file_editor_handle_keyboard_result(void)
{
    if (!ui_keyboard_was_confirmed()) {
        reset_cursor_blink();
        return;
    }

    const char *text = ui_keyboard_get_text();
    if (text == NULL) {
        reset_cursor_blink();
        return;
    }

    const char *old_text = fm_text_edit_get_line(s_editor.cursor_line);
    bool changed = strcmp(old_text, text) != 0;
    if (changed && fm_text_edit_set_line(s_editor.cursor_line, text)) {
        s_editor.dirty = true;
    }
    s_editor.cursor_column = ui_keyboard_get_cursor_column();

    adjust_scroll();
    reset_cursor_blink();
}

bool ui_file_editor_needs_tick(void)
{
    return !ui_keyboard_is_open() || s_editor.io_pending;
}

void ui_file_editor_open_actions(void)
{
    if (s_editor.io_pending) {
        return;
    }
    char title[EDITOR_TITLE_MAX];
    editor_make_title(title, sizeof(title));
    ui_popup_open_editor(title);
}

bool ui_file_editor_handle_popup_result(ui_popup_result_t result)
{
    switch (result) {
        case UI_POPUP_EDITOR_NEW_LINE:
            if (fm_text_edit_split_line(
                    s_editor.cursor_line,
                    s_editor.cursor_column)) {
                s_editor.cursor_line++;
                s_editor.cursor_column = 0;
                s_editor.dirty = true;
            } else {
                ui_popup_show_error("New line failed");
            }
            break;

        case UI_POPUP_EDITOR_BACKSPACE:
            if (fm_text_edit_backspace_at(
                    &s_editor.cursor_line,
                    &s_editor.cursor_column)) {
                s_editor.dirty = true;
            }
            break;

        case UI_POPUP_EDITOR_SAVE:
            editor_start_save(false);
            break;

        case UI_POPUP_EDITOR_SAVE_EXIT:
        case UI_POPUP_EDITOR_EXIT_SAVE:
            editor_start_save(true);
            break;

        case UI_POPUP_EDITOR_DISCARD:
            s_editor.dirty = false;
            fm_text_edit_close();
            return true;

        default:
            break;
    }

    adjust_scroll();
    reset_cursor_blink();
    return false;
}

bool ui_file_editor_handle_worker_event(const fm_worker_event_t *event)
{
    if (event == NULL || event->command != FM_CMD_SAVE_FILE ||
        !s_editor.io_pending) {
        return false;
    }

    s_editor.io_pending = false;
    if (event->type == FM_EVT_FILE_SAVED) {
        s_editor.dirty = false;
        if (s_editor.exit_after_save) {
            s_editor.exit_after_save = false;
            fm_text_edit_close();
            return true;
        }
    } else if (event->type == FM_EVT_ERROR) {
        s_editor.exit_after_save = false;
        ui_popup_show_error("Save failed");
    }

    reset_cursor_blink();
    return false;
}

bool ui_file_editor_handle_event(ui_event_t evt)
{
    if (s_editor.io_pending) {
        return false;
    }
    switch (evt) {
        case UI_EVT_UP:
            if (s_editor.cursor_line > 0) {
                s_editor.cursor_line--;
            }
            break;

        case UI_EVT_DOWN:
            if (s_editor.cursor_line + 1 < fm_text_edit_line_count()) {
                s_editor.cursor_line++;
            } else if (fm_text_edit_insert_line_after(s_editor.cursor_line)) {
                s_editor.cursor_line++;
                s_editor.dirty = true;
            }
            break;

        case UI_EVT_LEFT:
            if (s_editor.cursor_column > 0) {
                s_editor.cursor_column--;
            } else if (s_editor.cursor_line > 0) {
                s_editor.cursor_line--;
                s_editor.cursor_column =
                    fm_text_edit_line_length(s_editor.cursor_line);
            } else if (s_editor.dirty) {
                editor_open_exit_prompt();
            } else {
                fm_text_edit_close();
                return true;
            }
            break;

        case UI_EVT_RIGHT: {
            if (s_editor.cursor_column < FM_EDIT_MAX_LINE_LEN - 1) {
                s_editor.cursor_column++;
            }
            break;
        }

        case UI_EVT_SELECT:
            ui_keyboard_open_editor(
                fm_text_edit_get_line(s_editor.cursor_line),
                s_editor.cursor_column,
                s_editor.cursor_line
            );
            break;

        default:
            break;
    }

    adjust_scroll();
    reset_cursor_blink();
    return false;
}

void ui_file_editor_draw(gfx_canvas_t *canvas)
{
    if (canvas == NULL) {
        return;
    }

    gfx_canvas_fill(canvas, GFX_BLACK);

    char title[EDITOR_TITLE_MAX];
    editor_make_title(title, sizeof(title));
    gfx_canvas_draw_str(
        canvas,
        10,
        EDITOR_HEADER_BASELINE,
        title,
        UI_FONT,
        GFX_WHITE
    );
    const char *status = s_editor.io_pending
        ? "Saving..."
        : s_editor.dirty ? "*" : NULL;
    if (status != NULL) {
        int16_t status_width = gfx_canvas_measure_text_width(UI_FONT, status);
        gfx_canvas_draw_str(
            canvas,
            DISPLAY_WIDTH - status_width - 4,
            EDITOR_HEADER_BASELINE,
            status,
            UI_FONT,
            GFX_WHITE
        );
    }
    gfx_canvas_draw_line(
        canvas,
        0,
        EDITOR_HEADER_DIVIDER_Y,
        DISPLAY_WIDTH - 1,
        EDITOR_HEADER_DIVIDER_Y,
        GFX_WHITE
    );

    uint16_t total_lines = fm_text_edit_line_count();
    uint16_t visible_lines = editor_visible_lines();
    uint16_t visible_columns = editor_visible_columns();
    int16_t text_x = editor_text_x();

    TickType_t blink_ticks = pdMS_TO_TICKS(EDITOR_CURSOR_BLINK_MS);
    bool cursor_visible = blink_ticks == 0 ||
        ((xTaskGetTickCount() - s_cursor_epoch) / blink_ticks) % 2 == 0;

    for (uint16_t row = 0; row < visible_lines; row++) {
        uint16_t line_index = s_editor.scroll_y + row;
        if (line_index >= total_lines) {
            break;
        }

        int16_t baseline = EDITOR_BODY_BASELINE + row * EDITOR_LINE_HEIGHT;
        /* uint16_t needs at most five digits plus NUL. The field starts at
           three digits and grows with the document: 000, 1000, 10000. */
        char number[6];
        uint16_t number_digits = editor_line_number_digits();
        snprintf(number, sizeof(number), "%05u", (unsigned int)line_index);
        const char *visible_number = number + (5U - number_digits);
        gfx_canvas_draw_str(
            canvas,
            EDITOR_LINE_NUMBER_X,
            baseline,
            visible_number,
            UI_FONT,
            GFX_DARKGRAY
        );

        const char *line = fm_text_edit_get_line(line_index);
        size_t line_len = strnlen(line, FM_EDIT_MAX_LINE_LEN);
        if (s_editor.scroll_x < line_len) {
            size_t available = line_len - s_editor.scroll_x;
            size_t copy_len = available < visible_columns
                ? available
                : visible_columns;
            char visible[EDITOR_VISIBLE_TEXT_MAX];
            if (copy_len >= sizeof(visible)) {
                copy_len = sizeof(visible) - 1;
            }
            memcpy(visible, line + s_editor.scroll_x, copy_len);
            visible[copy_len] = '\0';
            gfx_canvas_draw_str(
                canvas,
                text_x,
                baseline,
                visible,
                UI_FONT,
                GFX_WHITE
            );
        }

        if (cursor_visible && line_index == s_editor.cursor_line) {
            int16_t cursor_x = text_x + editor_columns_pixel_width(
                line,
                s_editor.scroll_x,
                s_editor.cursor_column
            );

            if (cursor_x >= text_x && cursor_x < DISPLAY_WIDTH) {
                gfx_canvas_draw_line(
                    canvas,
                    cursor_x,
                    baseline - EDITOR_TEXT_ASCENT,
                    cursor_x,
                    baseline + EDITOR_TEXT_DESCENT,
                    GFX_WHITE
                );
            }
        }
    }
}
