#include "ui_keyboard.h"
#include <string.h>
#include <stdio.h>

#define KEYBOARD_MAX_LEN 128
#define GRID_ROWS 4
#define GRID_COLS 10

// Статический буфер — защита от Stack Overflow и динамической утечки памяти
static char s_buffer[KEYBOARD_MAX_LEN] = {0};
static uint8_t s_buf_pos = 0;
static bool s_is_open = false;
static bool s_confirmed = false;

static uint8_t s_row = 0;
static uint8_t s_col = 0;

// Сетка клавиатуры
static const char s_grid[GRID_ROWS][GRID_COLS] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '_'},
    {'z', 'x', 'c', 'v', 'b', 'n', 'm', '.', ' ', '<'} // '<' = Backspace
};

void ui_keyboard_open(const char *initial_text) {
    memset(s_buffer, 0, sizeof(s_buffer));
    s_buf_pos = 0;

    // Безопасное копирование начального текста с ограничением по длине
    if (initial_text != NULL) {
        strncpy(s_buffer, initial_text, KEYBOARD_MAX_LEN - 1);
        s_buffer[KEYBOARD_MAX_LEN - 1] = '\0';
        s_buf_pos = (uint8_t)strlen(s_buffer);
    }

    s_row = 0;
    s_col = 0;
    s_is_open = true;
    s_confirmed = false;
}

void ui_keyboard_close(void) {
    s_is_open = false;
}

bool ui_keyboard_is_open(void) {
    return s_is_open;
}

bool ui_keyboard_was_confirmed(void) {
    return s_confirmed;
}

// Защита: функция ГАРАНТИРОВАННО возвращает валидный указатель на строку (никогда не NULL)
const char *ui_keyboard_get_text(void) {
    return s_buffer;
}

void ui_keyboard_handle_event(ui_event_t evt) {
    if (!s_is_open) return;

    switch (evt) {
        case UI_EVT_UP:
            if (s_row > 0) s_row--;
            break;

        case UI_EVT_DOWN:
            if (s_row < GRID_ROWS - 1) s_row++;
            break;

        case UI_EVT_LEFT:
            if (s_col > 0) s_col--;
            break;

        case UI_EVT_RIGHT:
            if (s_col < GRID_COLS - 1) s_col++;
            break;

        case UI_EVT_SELECT: {
            char key = s_grid[s_row][s_col];

            if (key == '<') {
                // Backspace с защитой от ухода в отрицательные индексы
                if (s_buf_pos > 0) {
                    s_buf_pos--;
                    s_buffer[s_buf_pos] = '\0';
                }
            } else {
                // Вставка символа с СТРОГОЙ проверкой границы массива
                if (s_buf_pos < KEYBOARD_MAX_LEN - 1) {
                    s_buffer[s_buf_pos++] = key;
                    s_buffer[s_buf_pos] = '\0';
                }
            }
            break;
        }

#ifdef UI_EVT_BACK
        case UI_EVT_BACK:
            // Подтверждение ввода и закрытие
            s_confirmed = true;
            s_is_open = false;
            break;
#endif

        default:
            break;
    }
}

void ui_keyboard_draw(gfx_canvas_t *canvas) {
    if (!canvas || !s_is_open) return;
    // Отрисовка клавиатуры (использует только вычисленные локальные координаты)
}
