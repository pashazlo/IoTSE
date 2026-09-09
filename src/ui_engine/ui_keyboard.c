#include "ui_keyboard.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>

#include "ui_focus.h"
#include "display.h"
#include "assets/ibm_vga_font.h"
#include "esp_log.h"

static const char *TAG = "ui_keyboard";

// Шрифт клавиатуры.
#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)

// ============================================================================
// Раскладка клавиатуры
// ============================================================================

// Размер обычной клавиши.
#define KB_KEY_W 30
#define KB_KEY_H 20

// Расстояние между клавишами.
#define KB_GAP 2

// Y-координата первого ряда клавиатуры.
// Верхняя часть экрана остаётся под поле ввода текста.
#define KB_GRID_TOP 38

// Максимальное количество клавиш.
//
// Текущая раскладка использует:
//
// 10 + 10 + 9 + 10 + 4 = 43 клавиши.
//
// Оставляем запас на будущие символы/режимы раскладки.
#define KB_MAX_KEYS 50

// Типы специальных клавиш.
//
// Обычная клавиша:
//
// special == KB_SPECIAL_NONE
//
// и вставляет insert_char в текст.
//
// Специальные клавиши выполняют действие вместо вставки символа.
enum {
KB_SPECIAL_NONE = 0,
KB_SPECIAL_DEL,
KB_SPECIAL_OK,
KB_SPECIAL_ESC,
KB_SPECIAL_SPACE,
};

// Описание одной клавиши.
typedef struct {
char label[6]; // Текст, который рисуется на клавише.
char insert_char; // Символ для вставки обычной клавишей.
uint8_t special; // Тип специальной клавиши.
} kb_key_t;

// Список клавиш.
static kb_key_t s_keys[KB_MAX_KEYS];

// Геометрия клавиш.
//
// Хранится отдельно от данных клавиш, потому что ui_focus_find_nearest()
// нужен именно набор прямоугольников объектов.
static ui_bbox_t s_key_boxes[KB_MAX_KEYS];

static uint8_t s_key_count = 0;

// ============================================================================
// Построение раскладки
// ============================================================================

/**

@brief Добавить одну клавишу в раскладку.


Функция внутренняя и вызывается только при построении статичной

раскладки клавиатуры.
*/
static void add_key(
int16_t x,
int16_t y,
int16_t w,
int16_t h,
const char *label,
char ch,
uint8_t special
)
{
if (s_key_count >= KB_MAX_KEYS) {
ESP_LOGW(TAG, "Достигнут лимит клавиш (%d)", KB_MAX_KEYS);
return;
}

kb_key_t *key = &s_keys[s_key_count];

// Безопасно копируем отображаемую подпись клавиши.
strncpy(key->label, label, sizeof(key->label) - 1);
key->label[sizeof(key->label) - 1] = '\0';

key->insert_char = ch;
key->special = special;

// Геометрия клавиши для отрисовки и spatial-навигации.
ui_bbox_t *box = &s_key_boxes[s_key_count];

box->x = x;
box->y = y;
box->w = w;
box->h = h;

s_key_count++;
}

/**

@brief Добавить ряд обычных символьных клавиш.


Например:






add_row("QWERTY", 50, 10);



создаст 6 отдельных клавиш.
*/
static void add_row(const char *chars, int16_t y, int16_t x_start)
{
int16_t x = x_start;

for (const char *p = chars; *p != '\0'; p++) {

 char label[2] = {
     *p,
     '\0'
 };

 add_key(
     x,
     y,
     KB_KEY_W,
     KB_KEY_H,
     label,
     *p,
     KB_SPECIAL_NONE
 );

 x += KB_KEY_W + KB_GAP;

}
}

/**

@brief Построить раскладку один раз.


Раскладка статичная и не зависит от введённого текста, поэтому

пересчитывать её при каждом ui_keyboard_open() не нужно.
*/
static void build_layout_once(void)
{
if (s_key_count > 0) {
return;
}

// Ряд цифр.
add_row(
"1234567890",
KB_GRID_TOP,
2
);

// Верхний буквенный ряд.
add_row(
"QWERTYUIOP",
KB_GRID_TOP + (KB_KEY_H + KB_GAP),
2
);

// Средний ряд немного сдвинут вправо для визуального сходства
// с обычной QWERTY-клавиатурой.
add_row(
"ASDFGHJKL",
KB_GRID_TOP + (KB_KEY_H + KB_GAP) * 2,
2 + (KB_KEY_W + KB_GAP) / 2
);

// Нижний буквенный ряд + часто используемые символы.
add_row(
"ZXCVBNM.-_",
KB_GRID_TOP + (KB_KEY_H + KB_GAP) * 3,
2
);

// Ряд специальных клавиш.
int16_t special_y =
KB_GRID_TOP + (KB_KEY_H + KB_GAP) * 4;

add_key(
2,
special_y,
120,
KB_KEY_H,
"SPACE",
' ',
KB_SPECIAL_SPACE
);

add_key(
126,
special_y,
60,
KB_KEY_H,
"DEL",
0,
KB_SPECIAL_DEL
);

add_key(
190,
special_y,
60,
KB_KEY_H,
"OK",
0,
KB_SPECIAL_OK
);

add_key(
254,
special_y,
60,
KB_KEY_H,
"ESC",
0,
KB_SPECIAL_ESC
);

ESP_LOGI(TAG, "Раскладка клавиатуры построена: %u клавиш", s_key_count);
}

// ============================================================================
// Состояние клавиатуры
// ============================================================================

// Текущий редактируемый текст.
static char s_text[UI_KEYBOARD_MAX_TEXT_LEN];

// Длина текста без '\0'.
static uint8_t s_text_len = 0;

// Состояние модального окна.
static bool s_is_open = false;

// Результат последнего закрытия:
//
// true -> пользователь подтвердил OK.
// false -> отменил ESC/BACK/cancel.
static bool s_confirmed = false;

// Индекс текущей выбранной клавиши.
static uint8_t s_selected_key = 0;

// ============================================================================
// Внутренние операции с текстом
// ============================================================================

/**

@brief Добавить один ASCII-символ в конец текста.



Сейчас клавиатура ASCII, поэтому один символ == один байт.
*/
static void append_char(char ch)
{
if (s_text_len >= UI_KEYBOARD_MAX_TEXT_LEN - 1) {
ESP_LOGW(TAG, "Достигнут лимит длины текста (%d)", UI_KEYBOARD_MAX_TEXT_LEN - 1);
return;
}

s_text[s_text_len++] = ch;
s_text[s_text_len] = '\0';
}

/**

@brief Удалить последний ASCII-символ.


Важно: эта версия работает побайтно и корректна для текущей
ASCII-клавиатуры.


Для будущей UTF-8 клавиатуры потребуется отдельная логика удаления

последнего Unicode-символа.
*/
static void delete_last_char(void)
{
if (s_text_len == 0) {
return;
}

s_text_len--;
s_text[s_text_len] = '\0';
}

// ============================================================================
// Публичный API
// ============================================================================

void ui_keyboard_open(const char *initial_text)
{
build_layout_once();

// NULL означает пустой начальный текст.
const char *source =
    (initial_text != NULL) ? initial_text : "";


// Копируем начальный текст безопасно.
strncpy(
    s_text,
    source,
    UI_KEYBOARD_MAX_TEXT_LEN - 1
);

s_text[UI_KEYBOARD_MAX_TEXT_LEN - 1] = '\0';


// Вычисляем фактическую длину уже после возможного обрезания.
s_text_len = (uint8_t)strlen(s_text);


// Открываем модальное окно.
s_is_open = true;


// Старый результат предыдущего использования клавиатуры больше
// не имеет значения.
s_confirmed = false;


// Начинаем с первой клавиши.
s_selected_key = 0;


ESP_LOGI(TAG, "Клавиатура открыта");

}

void ui_keyboard_cancel(void)
{
if (!s_is_open) {
return;
}

s_is_open = false;
s_confirmed = false;

ESP_LOGI(TAG, "Клавиатура отменена");

}

bool ui_keyboard_is_open(void)
{
return s_is_open;
}

// ============================================================================
// Навигация по клавиатуре
// ============================================================================

static bool boxes_overlap_vertical(
    const ui_bbox_t *a,
    const ui_bbox_t *b
)
{
    return !(
        a->y + a->h <= b->y ||
        b->y + b->h <= a->y
    );
}


static bool boxes_overlap_horizontal(
    const ui_bbox_t *a,
    const ui_bbox_t *b
)
{
    return !(
        a->x + a->w <= b->x ||
        b->x + b->w <= a->x
    );
}


static int keyboard_find_next(
    uint8_t current,
    ui_event_t direction
)
{
    if (current >= s_key_count) {
        return -1;
    }

    const ui_bbox_t *cur = &s_key_boxes[current];

    int best_index = -1;
    int best_score = INT32_MAX;

    int cur_center_x = cur->x + cur->w / 2;
    int cur_center_y = cur->y + cur->h / 2;


    // ------------------------------------------------------------------------
    // Первый проход:
    //
    // Для LEFT/RIGHT предпочитаем клавиши того же ряда.
    //
    // Для UP/DOWN предпочитаем клавиши, пересекающиеся по X.
    //
    // Это критично для SPACE шириной 120 пикселей.
    // ------------------------------------------------------------------------

    for (int pass = 0; pass < 2; pass++) {

        best_index = -1;
        best_score = INT32_MAX;

        for (uint8_t i = 0; i < s_key_count; i++) {

            if (i == current) {
                continue;
            }

            const ui_bbox_t *candidate = &s_key_boxes[i];

            int cand_center_x =
                candidate->x + candidate->w / 2;

            int cand_center_y =
                candidate->y + candidate->h / 2;


            bool valid_direction = false;
            bool preferred_alignment = false;

            int primary_distance = 0;
            int secondary_distance = 0;


            switch (direction) {

                // ============================================================
                // LEFT
                // ============================================================

                case UI_EVT_LEFT:

                    if (cand_center_x >= cur_center_x) {
                        continue;
                    }

                    valid_direction = true;

                    preferred_alignment =
                        boxes_overlap_vertical(cur, candidate);

                    primary_distance =
                        cur_center_x - cand_center_x;

                    secondary_distance =
                        abs(cand_center_y - cur_center_y);

                    break;


                // ============================================================
                // RIGHT
                // ============================================================

                case UI_EVT_RIGHT:

                    if (cand_center_x <= cur_center_x) {
                        continue;
                    }

                    valid_direction = true;

                    preferred_alignment =
                        boxes_overlap_vertical(cur, candidate);

                    primary_distance =
                        cand_center_x - cur_center_x;

                    secondary_distance =
                        abs(cand_center_y - cur_center_y);

                    break;


                // ============================================================
                // UP
                // ============================================================

                case UI_EVT_UP:

                    if (cand_center_y >= cur_center_y) {
                        continue;
                    }

                    valid_direction = true;

                    preferred_alignment =
                        boxes_overlap_horizontal(cur, candidate);

                    primary_distance =
                        cur_center_y - cand_center_y;

                    secondary_distance =
                        abs(cand_center_x - cur_center_x);

                    break;


                // ============================================================
                // DOWN
                // ============================================================

                case UI_EVT_DOWN:

                    if (cand_center_y <= cur_center_y) {
                        continue;
                    }

                    valid_direction = true;

                    preferred_alignment =
                        boxes_overlap_horizontal(cur, candidate);

                    primary_distance =
                        cand_center_y - cur_center_y;

                    secondary_distance =
                        abs(cand_center_x - cur_center_x);

                    break;


                default:
                    continue;
            }


            if (!valid_direction) {
                continue;
            }


            // ----------------------------------------------------------------
            // Первый проход принимает только "логически выровненные" клавиши.
            //
            // Второй проход — fallback, если таких вообще нет.
            // ----------------------------------------------------------------

            if (pass == 0 && !preferred_alignment) {
                continue;
            }


            // Основное направление важнее бокового отклонения.
            int score =
                primary_distance * 100 +
                secondary_distance;


            if (score < best_score) {

                best_score = score;
                best_index = i;

            }
        }


        // Если на первом проходе нашли хороший объект —
        // сразу возвращаем его.
        if (best_index >= 0) {
            return best_index;
        }
    }


    return -1;
}

void ui_keyboard_handle_event(ui_event_t evt)
{
if (!s_is_open) {
return;
}

// Защита от повреждённого состояния.
//
// В нормальной работе build_layout_once() всегда создаёт клавиши
// до открытия клавиатуры.
if (s_key_count == 0 || s_selected_key >= s_key_count) {
    ESP_LOGE(TAG, "Некорректное состояние клавиатуры");
    ui_keyboard_cancel();
    return;
}


switch (evt) {

    // --------------------------------------------------------------------
    // Пространственная навигация
    // --------------------------------------------------------------------

    case UI_EVT_UP:
    case UI_EVT_DOWN:
    case UI_EVT_LEFT:
    case UI_EVT_RIGHT: {

        // Ищем ближайшую клавишу в указанном направлении.
        //
        // Это настоящая 2D-навигация:
        // клавиатура не хранит row/column, а использует геометрию
        // объектов через ui_focus_find_nearest().
       int next =
    keyboard_find_next(
        s_selected_key,
        evt
    );


        if (next >= 0) {
            s_selected_key = (uint8_t)next;
        }

        break;
    }


    // --------------------------------------------------------------------
    // Выбор клавиши
    // --------------------------------------------------------------------

    case UI_EVT_SELECT: {

        const kb_key_t *key =
            &s_keys[s_selected_key];


        switch (key->special) {

            // ------------------------------------------------------------
            // Подтверждение
            // ------------------------------------------------------------

            case KB_SPECIAL_OK:

                s_is_open = false;
                s_confirmed = true;

                ESP_LOGI(TAG, "Клавиатура подтверждена: '%s'", s_text);

                break;


            // ------------------------------------------------------------
            // Отмена
            // ------------------------------------------------------------

            case KB_SPECIAL_ESC:

                ui_keyboard_cancel();

                break;


            // ------------------------------------------------------------
            // Удаление последнего символа
            // ------------------------------------------------------------

            case KB_SPECIAL_DEL:

                delete_last_char();

                break;


            // ------------------------------------------------------------
            // Пробел
            // ------------------------------------------------------------

            case KB_SPECIAL_SPACE:

                append_char(' ');

                break;


            // ------------------------------------------------------------
            // Обычный символ
            // ------------------------------------------------------------

            case KB_SPECIAL_NONE:
            default:

                append_char(key->insert_char);

                break;
        }

        break;
    }


    // --------------------------------------------------------------------
    // BACK
    //
    // Если в твоём ui_event.h существует UI_EVT_BACK, можно
    // использовать физическую кнопку BACK как DEL.
    //
    // Если такого события нет — этот case просто нужно убрать.
    // --------------------------------------------------------------------
    default:
        break;
}

}

bool ui_keyboard_was_confirmed(void)
{
return s_confirmed;
}

const char *ui_keyboard_get_text(void)
{
return s_text;
}

// ============================================================================
// Отрисовка
// ============================================================================

void ui_keyboard_draw(gfx_canvas_t *canvas)
{
if (!s_is_open || canvas == NULL) {
return;
}

// ------------------------------------------------------------------------
// Фон модального окна
// ------------------------------------------------------------------------

gfx_canvas_fill_rect(
    canvas,
    0,
    0,
    DISPLAY_WIDTH,
    DISPLAY_HEIGHT,
    0x0000
);


gfx_canvas_draw_rect(
    canvas,
    0,
    0,
    DISPLAY_WIDTH,
    DISPLAY_HEIGHT,
    0xFFFF
);


// ------------------------------------------------------------------------
// Поле ввода
// ------------------------------------------------------------------------

gfx_canvas_draw_str(
    canvas,
    6,
    16,
    s_text,
    UI_FONT,
    0xFFFF
);


// Статичный курсор после последнего символа.
int16_t cursor_x =
    6 + gfx_canvas_measure_text_width(
        UI_FONT,
        s_text
    );


gfx_canvas_draw_line(
    canvas,
    cursor_x,
    6,
    cursor_x,
    20,
    0xFFFF
);


// ------------------------------------------------------------------------
// Клавиши
// ------------------------------------------------------------------------

for (uint8_t i = 0; i < s_key_count; i++) {

    const ui_bbox_t *box =
        &s_key_boxes[i];


    bool selected =
        (i == s_selected_key);


    // Выбранная клавиша инвертируется.
    if (selected) {

        gfx_canvas_fill_rect(
            canvas,
            box->x,
            box->y,
            box->w,
            box->h,
            0xFFFF
        );

    } else {

        gfx_canvas_draw_rect(
            canvas,
            box->x,
            box->y,
            box->w,
            box->h,
            0x8410
        );
    }


    uint16_t text_color =
        selected ? 0x0000 : 0xFFFF;


    gfx_canvas_draw_str(
        canvas,
        box->x + 4,
        box->y + box->h - 5,
        s_keys[i].label,
        UI_FONT,
        text_color
    );
}

}
