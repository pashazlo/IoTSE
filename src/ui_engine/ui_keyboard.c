#include "ui_keyboard.h"
#include "assets/ibm_vga_font.h"
#include <stdio.h>
#include <string.h>

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)
#define GRID_ROWS 4
#define GRID_COLS 10
#define KEYBOARD_PREVIEW_MAX 32
#define KEYBOARD_NAME_MAX 64
static char s_buffer[UI_KEYBOARD_MAX_TEXT_LEN];
static size_t s_buf_pos;
static size_t s_input_limit;
static bool s_is_open, s_confirmed;
static bool s_editor_mode;
static bool s_masked;
static const char *s_prompt;
static uint8_t s_char_mode;
static uint16_t s_editor_line_number;
static uint8_t s_row, s_col;
static const char s_grids[3][GRID_ROWS][GRID_COLS] = {{
    {'1','2','3','4','5','6','7','8','9','0'},
    {'q','w','e','r','t','y','u','i','o','p'},
    {'a','s','d','f','g','h','j','k','l','_'},
    {'z','x','c','v','b','n','m','.',' ','<'}
}, {
    {'1','2','3','4','5','6','7','8','9','0'},
    {'Q','W','E','R','T','Y','U','I','O','P'},
    {'A','S','D','F','G','H','J','K','L','_'},
    {'Z','X','C','V','B','N','M','.',' ','<'},
}, {
    {'!','@','#','$','%','^','&','*','(',')'},
    {'-','+','=','[',']','{','}',';',':','/'},
    {'\\','|','?',',','.','<','>','~','`','_'},
    {'"','\'','0','1','2','3','4','5','6','<'},
}};

void ui_keyboard_open(const char *initial_text)
{
    ui_keyboard_open_prompt("Text", initial_text, KEYBOARD_NAME_MAX - 1, false);
}

void ui_keyboard_open_prompt(
    const char *prompt,
    const char *initial_text,
    size_t max_text_len,
    bool masked
)
{
    if (!initial_text) initial_text = "";
    strncpy(s_buffer, initial_text, sizeof(s_buffer) - 1);
    s_buffer[sizeof(s_buffer) - 1] = '\0';
    s_buf_pos = strlen(s_buffer);
    s_row = s_col = 0;
    s_confirmed = false;
    s_editor_mode = false;
    s_masked = masked;
    s_prompt = prompt != NULL ? prompt : "Text";
    s_char_mode = 0;
    s_input_limit = max_text_len < sizeof(s_buffer)
        ? max_text_len
        : sizeof(s_buffer) - 1;
    s_is_open = true;
}

void ui_keyboard_open_editor(
    const char *line_text,
    uint16_t cursor_column,
    uint16_t line_number
)
{
    if (line_text == NULL) line_text = "";
    strncpy(s_buffer, line_text, sizeof(s_buffer) - 1);
    s_buffer[sizeof(s_buffer) - 1] = '\0';

    s_buf_pos = cursor_column < sizeof(s_buffer)
        ? cursor_column
        : sizeof(s_buffer) - 1;
    s_editor_line_number = line_number;
    s_row = s_col = 0;
    s_confirmed = false;
    s_editor_mode = true;
    s_masked = false;
    s_prompt = "Text";
    s_char_mode = 0;
    s_input_limit = sizeof(s_buffer) - 1;
    s_is_open = true;
}
void ui_keyboard_cancel(void) { s_confirmed = false; s_is_open = false; }
bool ui_keyboard_is_open(void) { return s_is_open; }
bool ui_keyboard_was_confirmed(void) { return s_confirmed; }
const char *ui_keyboard_get_text(void) { return s_buffer; }
uint16_t ui_keyboard_get_cursor_column(void) { return (uint16_t)s_buf_pos; }

void ui_keyboard_handle_event(ui_event_t evt)
{
    if (!s_is_open) return;
    switch (evt) {
    case UI_EVT_UP:
        if (s_row) { if (s_row == GRID_ROWS) s_col = s_col * GRID_COLS / 4; s_row--; }
        break;
    case UI_EVT_DOWN:
        if (s_row < GRID_ROWS) {
            s_row++;
            if (s_row == GRID_ROWS) { s_col = s_col * 4 / GRID_COLS; if (s_col > 3) s_col = 3; }
        }
        break;
    case UI_EVT_LEFT: if (s_col) s_col--; break;
    case UI_EVT_RIGHT:
        if (s_col + 1 < (s_row == GRID_ROWS ? 4 : GRID_COLS)) s_col++;
        break;
    case UI_EVT_SELECT:
        if (s_row == GRID_ROWS) {
            if (s_col == 0) { s_confirmed = true; s_is_open = false; }
            else if (s_col == 1) ui_keyboard_cancel();
            else if (s_col == 2) { s_buf_pos = 0; s_buffer[0] = '\0'; }
            else { s_char_mode = (s_char_mode + 1U) % 3U; }
        } else {
            char ch = s_grids[s_char_mode][s_row][s_col];
            if (ch == '<') {
                if (s_buf_pos) {
                    if (s_editor_mode) {
                        size_t line_len = strlen(s_buffer);
                        if (s_buf_pos > line_len) {
                            s_buf_pos--;
                        } else {
                            memmove(
                                s_buffer + s_buf_pos - 1,
                                s_buffer + s_buf_pos,
                                line_len - s_buf_pos + 1
                            );
                            s_buf_pos--;
                        }
                    } else {
                        s_buffer[--s_buf_pos] = '\0';
                    }
                }
            } else {
                if (strlen(s_buffer) >= s_input_limit) {
                    break;
                }
                if (s_editor_mode) {
                    size_t line_len = strlen(s_buffer);
                    if (s_buf_pos > line_len) {
                        size_t padding = s_buf_pos - line_len;
                        if (s_buf_pos >= sizeof(s_buffer) - 1) {
                            break;
                        }
                        memset(s_buffer + line_len, ' ', padding);
                        s_buffer[s_buf_pos] = '\0';
                        line_len = s_buf_pos;
                    }
                    memmove(
                        s_buffer + s_buf_pos + 1,
                        s_buffer + s_buf_pos,
                        line_len - s_buf_pos + 1
                    );
                    s_buffer[s_buf_pos++] = ch;
                } else {
                    s_buffer[s_buf_pos++] = ch;
                    s_buffer[s_buf_pos] = '\0';
                }
            }
        }
        break;
    default: break;
    }
}

static void draw_key(gfx_canvas_t *c, int x, int y, int w,
                     const char *label, bool selected)
{
    const uint16_t border = selected ? 0xFFFF : 0x4208;
    const uint16_t text = selected ? 0xFFFF : 0xC618;

    gfx_canvas_draw_round_rect(c, x, y, w, 20, 3, border);
    if (selected && w > 8) {
        gfx_canvas_draw_round_rect(c, x + 2, y + 2, w - 4, 16, 3, 0x7BEF);
    }
    int tw = gfx_canvas_measure_text_width(UI_FONT, label);
    gfx_canvas_draw_str(c, x + (w - tw) / 2, y + 14, label,
                        UI_FONT, text);
}
void ui_keyboard_draw(gfx_canvas_t *c)
{
    if (!c || !s_is_open) return;
    gfx_canvas_fill(c, 0x0000);
    gfx_canvas_draw_str(c, 8, 14, s_prompt, UI_FONT, 0xFFFF);
    if (s_editor_mode) {
        char number[7];
        snprintf(
            number,
            sizeof(number),
            "%05u ",
            (unsigned int)s_editor_line_number
        );
        uint16_t digits = s_editor_line_number >= 10000U ? 5 :
                          s_editor_line_number >= 1000U ? 4 : 3;
        const char *visible_number = number + (5U - digits);
        gfx_canvas_draw_str(c, 8, 36, visible_number, UI_FONT, 0x7BEF);

        int16_t text_x = 8 + gfx_canvas_measure_text_width(UI_FONT, visible_number);
        uint16_t cell_width = UI_FONT->glyphs[' ' - UI_FONT->first].xAdvance;
        uint16_t visible_columns = cell_width > 0
            ? (uint16_t)((311 - text_x) / cell_width)
            : 1;
        if (visible_columns == 0) visible_columns = 1;

        size_t half_window = visible_columns / 2;
        size_t start = s_buf_pos > half_window
            ? s_buf_pos - half_window
            : 0;
        if (start + visible_columns > UI_KEYBOARD_MAX_TEXT_LEN - 1) {
            start = UI_KEYBOARD_MAX_TEXT_LEN - 1 - visible_columns;
        }

        char preview[KEYBOARD_PREVIEW_MAX];
        size_t copy_len = visible_columns;
        if (copy_len >= sizeof(preview)) copy_len = sizeof(preview) - 1;
        size_t line_len = strlen(s_buffer);
        for (size_t i = 0; i < copy_len; i++) {
            size_t source = start + i;
            preview[i] = source < line_len ? s_buffer[source] : ' ';
        }
        preview[copy_len] = '\0';
        gfx_canvas_draw_str(c, text_x, 36, preview, UI_FONT, 0xFFFF);

        int16_t cursor_x = text_x + (int16_t)((s_buf_pos - start) * cell_width);
        if (cursor_x <= 311) {
            gfx_canvas_draw_line(c, cursor_x, 24, cursor_x, 39, 0xFFFF);
        }
    } else {
        /* Show the tail so newly typed characters remain visible. */
        size_t start = s_buf_pos > 18 ? s_buf_pos - 18 : 0;
        char preview[20];
        size_t visible = strlen(s_buffer + start);
        if (visible >= sizeof(preview)) visible = sizeof(preview) - 1;
        if (s_masked) {
            memset(preview, '*', visible);
            preview[visible] = '\0';
        } else {
            memcpy(preview, s_buffer + start, visible);
            preview[visible] = '\0';
        }
        gfx_canvas_draw_str(c, 8, 36, preview, UI_FONT, 0xFFFF);
    }
    gfx_canvas_draw_line(c, 8, 42, 311, 42, 0x8410);
    for (int r = 0; r < GRID_ROWS; r++) {
        for (int col = 0; col < GRID_COLS; col++) {
            char label[2] = {s_grids[s_char_mode][r][col], '\0'};
            draw_key(c, 2 + col * 32, 48 + r * 22, 28, label,
                     s_row == r && s_col == col);
        }
    }
    static const char *mode_names[] = {"abc", "ABC", "#+="};
    const char *actions[] = {"OK", "ESC", "CLEAR", mode_names[s_char_mode]};
    for (int i = 0; i < 4; i++)
        draw_key(c, 2 + i * 79, 140, 75, actions[i],
                 s_row == GRID_ROWS && s_col == i);
}
