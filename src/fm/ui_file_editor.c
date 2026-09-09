#include "ui_file_editor.h"
#include "fm_text_edit.h"
#include "ui_keyboard.h"
#include "display.h"
#include "assets/ibm_vga_font.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)
#define CHAR_WIDTH    8
#define CHAR_HEIGHT   14
#define VISIBLE_LINES 14
#define VISIBLE_COLS  30

typedef struct {
    uint16_t cursor_line;
    uint16_t cursor_column;
    uint16_t scroll_y;
    uint16_t scroll_x;
} ui_editor_state_t;

static ui_editor_state_t s_editor;

// Safe bounds clamping helper
static inline int16_t clamp_i16(int16_t val, int16_t min, int16_t max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

static void adjust_scroll(void) {
    // 1. Корректировка вертикального скролла
    if (s_editor.cursor_line < s_editor.scroll_y) {
        s_editor.scroll_y = s_editor.cursor_line;
    } else if (s_editor.cursor_line >= s_editor.scroll_y + VISIBLE_LINES) {
        s_editor.scroll_y = s_editor.cursor_line - VISIBLE_LINES + 1;
    }

    // 2. Корректировка горизонтального скролла
    if (s_editor.cursor_column < s_editor.scroll_x) {
        s_editor.scroll_x = s_editor.cursor_column;
    } else if (s_editor.cursor_column >= s_editor.scroll_x + VISIBLE_COLS) {
        s_editor.scroll_x = s_editor.cursor_column - VISIBLE_COLS + 1;
    }
}

void ui_file_editor_init(void) {
    memset(&s_editor, 0, sizeof(s_editor));
    fm_text_edit_init();
}

void ui_file_editor_open_file(const char *filepath) {
    if (!filepath) return;
    memset(&s_editor, 0, sizeof(s_editor));
    fm_text_edit_open(filepath);
}

void ui_file_editor_handle_event(ui_event_t evt) {

    // 1. Модальная клавиатура (перехватывает управление)
    if (ui_keyboard_is_open()) {
        ui_keyboard_handle_event(evt);

        // Если клавиатуру только что закрыли
        if (!ui_keyboard_is_open()) {
            if (ui_keyboard_was_confirmed()) {
                const char *text = ui_keyboard_get_text();
                
                // Защита от NULL указателя
                if (text != NULL && text[0] != '\0') {
                    size_t len = strlen(text);
                    if (fm_text_edit_insert_text(s_editor.cursor_line, s_editor.cursor_column, text)) {
                        s_editor.cursor_column += (uint16_t)len;
                    }
                }
            }
            adjust_scroll();
        }
        return;
    }

    // 2. Обработка навигации
    switch (evt) {
        case UI_EVT_UP:
            if (s_editor.cursor_line > 0) {
                s_editor.cursor_line--;
            }
            break;

        case UI_EVT_DOWN:
            if (s_editor.cursor_line < FM_EDIT_MAX_LINES - 1) {
                s_editor.cursor_line++;
                fm_text_edit_ensure_line(s_editor.cursor_line);
            }
            break;

        case UI_EVT_LEFT:
            if (s_editor.cursor_column > 0) {
                s_editor.cursor_column--;
            } else if (s_editor.cursor_line > 0) {
                // Переход в конец предыдущей строки при нажатии влево
                s_editor.cursor_line--;
                s_editor.cursor_column = fm_text_edit_line_length(s_editor.cursor_line);
            }
            break;

        case UI_EVT_RIGHT:
            if (s_editor.cursor_column < FM_EDIT_MAX_LINE_LEN - 1) {
                s_editor.cursor_column++;
            }
            break;

        case UI_EVT_SELECT:
            // Открываем клавиатуру
            ui_keyboard_open("");
            break;

#ifdef UI_EVT_BACK
        case UI_EVT_BACK:
            if (s_editor.cursor_column > 0) {
                if (fm_text_edit_backspace(s_editor.cursor_line, s_editor.cursor_column)) {
                    s_editor.cursor_column--;
                }
            }
            break;
#endif

        default:
            break;
    }

    adjust_scroll();
}

void ui_file_editor_draw(gfx_canvas_t *canvas) {
    if (!canvas) return;

    // Очистка экрана
    gfx_canvas_fill_rect(canvas, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0x0000);

    uint16_t total_lines = fm_text_edit_line_count();
    int16_t y = 10;
    int16_t text_x_offset = 35; // Отступ под номера строк

    for (uint16_t line_idx = s_editor.scroll_y; 
         line_idx < total_lines && (y + CHAR_HEIGHT) <= DISPLAY_HEIGHT; 
         line_idx++) 
    {
        // 1. Отрисовка номера строки с защитой буфера
        char num_buf[10];
        snprintf(num_buf, sizeof(num_buf), "%03u", (unsigned int)(line_idx + 1));
        gfx_canvas_draw_str(canvas, 2, y + CHAR_HEIGHT - 2, num_buf, UI_FONT, 0x8410);

        // 2. Отрисовка текста строки
        const char *full_line = fm_text_edit_get_line(line_idx);
        if (full_line) {
            size_t line_len = strlen(full_line);

            if (s_editor.scroll_x < line_len) {
                const char *visible_text = full_line + s_editor.scroll_x;
                gfx_canvas_draw_str(canvas, text_x_offset, y + CHAR_HEIGHT - 2, visible_text, UI_FONT, 0xFFFF);
            }
        }

        // 3. Безопасная отрисовка курсора с клиппингом координат
        if (line_idx == s_editor.cursor_line) {
            int32_t rel_col = (int32_t)s_editor.cursor_column - (int32_t)s_editor.scroll_x;
            
            if (rel_col >= 0 && rel_col < VISIBLE_COLS) {
                int16_t cursor_x = text_x_offset + (int16_t)(rel_col * CHAR_WIDTH);
                
                // Проверяем, что координаты курсора лежат строго в пределах дисплея
                if (cursor_x >= 0 && cursor_x < DISPLAY_WIDTH) {
                    int16_t y1 = clamp_i16(y, 0, DISPLAY_HEIGHT - 1);
                    int16_t y2 = clamp_i16(y + CHAR_HEIGHT, 0, DISPLAY_HEIGHT - 1);
                    
                    gfx_canvas_draw_line(canvas, cursor_x, y1, cursor_x, y2, 0x07E0); // Зеленый курсор
                }
            }
        }

        y += CHAR_HEIGHT + 2;
    }

    // 4. Модальный слой клавиатуры рисуется в самом конце
    if (ui_keyboard_is_open()) {
        ui_keyboard_draw(canvas);
    }
}
