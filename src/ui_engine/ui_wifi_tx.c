#include "ui_wifi_tx.h"

#include <stdio.h>
#include <string.h>

#include "assets/ibm_vga_font.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_cursor.h"
#include "ui_focus.h"
#include "ui_keyboard.h"  /* модальная экранная клавиатура для правки SSID */
#include "ui_render.h"
#include "ui_screen.h"
#include "ui_wifi.h"      /* ui_wifi_has_focus() — виджет статуса подключения */
#include "tx_config.h"    /* модель данных настроек пакета (нижний слой) */

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)

/* --- Геометрия строки ---
 * Метка слева (ROW_LABEL_X), значение — в своём «окне» справа от ROW_VALUE_X до
 * ROW_VIEW_RIGHT. Если значение шире окна, оно прокручивается внутри него. */
#define ROW_LABEL_X     20
#define ROW_VALUE_X     150
#define ROW_VIEW_RIGHT  310
#define ROW_VALUE_VIEW  (ROW_VIEW_RIGHT - ROW_VALUE_X) /* ширина окна значения, px */
#define ROW_FIRST_Y     44
#define ROW_STEP_Y      22
#define ROW_BACK_GAP    12   /* зазор перед «< BACK», как в обычных подменю */

/* Отступы рамки-курсора вокруг текста — те же, что в ui_render.c, чтобы вид
 * выделения был единым по всему интерфейсу. */
#define CURSOR_PAD      2
#define TEXT_ASCENT     11
#define TEXT_DESCENT    3

/* Прокрутка длинного значения: пауза в начале, равномерный сдвиг, пауза в конце,
 * затем цикл повторяется. Те же тайминги, что у других экранов проекта. */
#define SCROLL_START_MS      1000U
#define SCROLL_END_MS        1200U
#define SCROLL_MS_PER_PIXEL  25U   /* 40 пикселей в секунду */

/* Строки экрана: 4 настройки + «назад». Именованные индексы — один источник
 * правды для обработчика и отрисовки. */
enum {
    TX_ROW_SSID = 0,
    TX_ROW_REASON,
    TX_ROW_BURST,
    TX_ROW_CHANNEL,
    TX_ROW_BACK,
    TX_ROW_COUNT, /* всегда последний — это и есть число строк */
};

/* Состояние прокрутки ТОЛЬКО выбранной строки (одновременно крутится максимум
 * одно значение — то, что под курсором). */
static struct {
    char       text[48];     /* значение, по которому сейчас считаем сдвиг */
    uint32_t   elapsed_ms;   /* сколько прошло в текущем цикле */
    TickType_t last_tick;    /* для вычисления дельты времени между кадрами */
    int16_t    offset_px;    /* текущий горизонтальный сдвиг значения */
    bool       active;       /* значение не влезает в окно → нужна прокрутка */
} s_scroll;

/* Режим правки: true — выбранная числовая строка сейчас редактируется (её
 * значение меняют кнопками LEFT/RIGHT), false — обычная навигация по строкам.
 * Редактируется всегда та строка, что под курсором (ui_focus_get), поэтому
 * отдельно её индекс не храним. */
static bool s_editing;

/* Какие строки — редактируемые числа. SSID правится клавиатурой (следующий шаг),
 * Channel пока только Auto (не редактируем), «< BACK» — не значение. */
static bool row_is_numeric(uint8_t row)
{
    return row == TX_ROW_REASON || row == TX_ROW_BURST;
}

/* Изменить значение редактируемой строки на delta (+1/−1). Границы держат сами
 * сеттеры tx_config, поэтому здесь достаточно защититься от ухода в минус перед
 * приведением к беззнаковому типу. */
static void edit_adjust(uint8_t row, int delta)
{
    if (row == TX_ROW_REASON) {
        int v = (int)tx_config_deauth_reason() + delta;
        if (v < 0) {
            v = 0; /* сеттер поднимет до минимума */
        }
        tx_config_set_deauth_reason((uint16_t)v);
    } else if (row == TX_ROW_BURST) {
        int v = (int)tx_config_burst_count() + delta;
        if (v < 0) {
            v = 0;
        }
        tx_config_set_burst_count((uint8_t)v);
    }
}

/* y базовой линии строки i (с зазором перед «< BACK»). */
static int16_t row_baseline(uint8_t i)
{
    return ROW_FIRST_Y + (int16_t)i * ROW_STEP_Y +
           (i == TX_ROW_BACK ? ROW_BACK_GAP : 0);
}

/* Пересчитать прокрутку для значения выбранной строки. Логика один-в-один с
 * других экранов: пока значение влезает в окно — сдвига нет; если шире — гоняем
 * по циклу пауза→сдвиг→пауза. */
static void update_scroll(const char *value)
{
    TickType_t now = xTaskGetTickCount();

    /* Значение под курсором сменилось (переключили строку/поменяли настройку) —
     * начинаем цикл заново с паузы. */
    bool changed = strcmp(s_scroll.text, value) != 0;
    if (changed) {
        snprintf(s_scroll.text, sizeof(s_scroll.text), "%s", value);
        s_scroll.elapsed_ms = 0;
        s_scroll.offset_px = 0;
    }

    /* На сколько пикселей значение вылезает за окно. <=0 — влезает, прокрутки нет. */
    int16_t distance = gfx_canvas_measure_text_width(UI_FONT, value) - ROW_VALUE_VIEW;
    s_scroll.active = distance > 0;

    if (s_scroll.active && !changed) {
        /* Дельту времени считаем вычитанием тиков — корректно и при переполнении
         * счётчика (разность беззнаковых тиков даёт верный интервал). */
        uint64_t delta = (uint64_t)(TickType_t)(now - s_scroll.last_tick) *
                         1000U / configTICK_RATE_HZ;
        uint32_t move_ms = (uint32_t)distance * SCROLL_MS_PER_PIXEL;
        uint32_t cycle = SCROLL_START_MS + move_ms + SCROLL_END_MS;
        s_scroll.elapsed_ms = (uint32_t)((s_scroll.elapsed_ms + delta) % cycle);

        uint32_t t = s_scroll.elapsed_ms;
        if (t <= SCROLL_START_MS) {
            s_scroll.offset_px = 0;                      /* пауза в начале */
        } else if (t >= SCROLL_START_MS + move_ms) {
            s_scroll.offset_px = distance;               /* доехали до конца, пауза */
        } else {
            s_scroll.offset_px =
                (int16_t)((t - SCROLL_START_MS) / SCROLL_MS_PER_PIXEL); /* едем */
        }
    } else if (!s_scroll.active) {
        s_scroll.elapsed_ms = 0;
        s_scroll.offset_px = 0;
    }
    s_scroll.last_tick = now;
}

/* Нарисовать значение в окне [ROW_VALUE_X .. ROW_VIEW_RIGHT) со сдвигом offset.
 * Рисуем по пикселям и жёстко клиппируем по окну: обычный draw_str клиппирует
 * только по экрану, и сдвинутый текст залез бы на соседние области. */
static void draw_value_clipped(gfx_canvas_t *canvas, int16_t baseline,
                               const char *value, int16_t offset, uint16_t color)
{
    int16_t pen_x = ROW_VALUE_X - offset;
    const unsigned char *text = (const unsigned char *)value;
    for (; *text != '\0'; ++text) {
        if (*text < UI_FONT->first || *text > UI_FONT->last) continue;
        const gfx_glyph_t *glyph = &UI_FONT->glyphs[*text - UI_FONT->first];
        for (int yy = 0; yy < glyph->height; ++yy) {
            for (int xx = 0; xx < glyph->width; ++xx) {
                int16_t x = pen_x + glyph->xOffset + xx;
                int16_t y = baseline + glyph->yOffset + yy;
                if (x < ROW_VALUE_X || x >= ROW_VIEW_RIGHT ||
                    y < baseline - TEXT_ASCENT || y > baseline + TEXT_DESCENT) {
                    continue;
                }
                size_t bit = (size_t)yy * glyph->width + xx;
                if (UI_FONT->bitmap[glyph->bitmapOffset + bit / 8U] &
                    (0x80U >> (bit % 8U))) {
                    gfx_canvas_draw_pixel(canvas, x, y, color);
                }
            }
        }
        pen_x += glyph->xAdvance;
        if (pen_x >= ROW_VIEW_RIGHT) break;
    }
}

/* --- Открытие экрана --- */
void ui_wifi_tx_open(void)
{
    ui_screen_set(UI_SCREEN_WIFI_TX);
    ui_focus_reset(UI_FOCUS_WIFI_TX);        /* начинаем с первой строки */
    memset(&s_scroll, 0, sizeof(s_scroll));  /* прокрутка с чистого листа */
    s_editing = false;                       /* входим в режиме навигации */
}

/* --- Обработка кнопок ---
 * Два режима:
 *   обычный — UP/DOWN ходят по строкам, SELECT на «< BACK» выходит с экрана,
 *             SELECT на числовой строке входит в правку;
 *   правка  — LEFT/RIGHT меняют значение (−1/+1), SELECT подтверждает и выходит,
 *             UP/DOWN игнорируются (чтобы не соскочить со строки). */
void ui_wifi_tx_handle_event(ui_event_t event, gfx_canvas_t *canvas)
{
    uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_TX);

    if (s_editing) {
        if (event == UI_EVT_LEFT) {
            edit_adjust(selected, -1);
            ui_render(canvas);
        } else if (event == UI_EVT_RIGHT) {
            edit_adjust(selected, +1);
            ui_render(canvas);
        } else if (event == UI_EVT_SELECT) {
            s_editing = false; /* подтвердить и вернуться к навигации */
            ui_render(canvas);
        }
        return; /* в режиме правки другие кнопки ничего не делают */
    }

    if (event == UI_EVT_UP || event == UI_EVT_DOWN) {
        ui_focus_move(UI_FOCUS_WIFI_TX, TX_ROW_COUNT, event);
        memset(&s_scroll, 0, sizeof(s_scroll)); /* новая строка — пауза заново */
        ui_render(canvas);
    } else if (event == UI_EVT_SELECT) {
        if (selected == TX_ROW_BACK) {
            /* Выход с экрана — единственная точка покидания настроек, поэтому
             * здесь же сохраняем их в NVS одним разом (а не на каждый шаг +/−,
             * чтобы беречь ресурс флеша). Ошибку записи молча не прячем: её
             * залогирует сам tx_config_save(); на навигацию она не влияет. */
            (void)tx_config_save();
            ui_screen_set(UI_SCREEN_WIFI_MENU);
            ui_render(canvas);
        } else if (selected == TX_ROW_SSID) {
            /* Открываем модальную клавиатуру с текущим SSID. Введённый текст
             * применит ui_wifi_tx_handle_keyboard_result() после её закрытия
             * (вызов делает контроллер). max_text_len = TX_CONFIG_SSID_MAX_LEN,
             * masked = false (имя сети не секрет). */
            ui_keyboard_open_prompt("Beacon SSID", tx_config_ssid(),
                                    TX_CONFIG_SSID_MAX_LEN, false);
            ui_render(canvas);
        } else if (row_is_numeric(selected)) {
            s_editing = true; /* войти в правку числовой строки */
            ui_render(canvas);
        }
        /* Channel на этом шаге по SELECT ничего не делает (только Auto). */
    }
}

/* Применить текст из клавиатуры к SSID. Зовётся контроллером ПОСЛЕ закрытия
 * клавиатуры (когда ui_keyboard_is_open() снова false). На отмену (ESC) ничего
 * не меняем; пустую/слишком длинную строку отфильтрует сам сеттер. */
void ui_wifi_tx_handle_keyboard_result(void)
{
    if (!ui_keyboard_was_confirmed()) {
        return; /* отмена — SSID оставляем как был */
    }
    tx_config_set_ssid(ui_keyboard_get_text());
}

/* --- Отрисовка --- */
void ui_wifi_tx_draw(gfx_canvas_t *canvas, ui_cursor_t *cursor)
{
    gfx_canvas_fill(canvas, GFX_BLACK);
    gfx_canvas_draw_str(canvas, 10, 12, "TX ESP - Packet", UI_FONT, GFX_WHITE);
    gfx_canvas_draw_line(canvas, 0, 18, DISPLAY_WIDTH - 1, 18, GFX_WHITE);

    /* Текстовый вид значений. Канал 0 показываем как "Auto" (текущий канал радио). */
    char reason[8];
    char burst[8];
    char channel[8];
    snprintf(reason, sizeof(reason), "%u", (unsigned)tx_config_deauth_reason());
    snprintf(burst, sizeof(burst), "%u", (unsigned)tx_config_burst_count());
    uint8_t ch = tx_config_channel();
    if (ch == TX_CONFIG_CHANNEL_AUTO) {
        snprintf(channel, sizeof(channel), "Auto");
    } else {
        snprintf(channel, sizeof(channel), "%u", (unsigned)ch);
    }

    const char *labels[TX_ROW_COUNT] = {
        "SSID", "Reason", "Burst", "Channel", "< BACK",
    };
    const char *values[TX_ROW_COUNT] = {
        tx_config_ssid(), reason, burst, channel, "",
    };

    uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_TX);

    /* Если выбранная числовая строка в правке — покажем значение в «стрелках»
     * < .. >: сами стрелки и есть признак режима правки (цвет обычный). Готовим
     * строку заранее: она же задаёт ширину рамки-курсора ниже. */
    bool editing_numeric = s_editing && row_is_numeric(selected);
    char edit_buf[16] = "";
    if (editing_numeric) {
        snprintf(edit_buf, sizeof(edit_buf), "< %s >", values[selected]);
    }

    for (uint8_t i = 0; i < TX_ROW_COUNT; ++i) {
        int16_t y = row_baseline(i);
        uint16_t color = (i == selected) ? GFX_WHITE : GFX_DARKGRAY;
        gfx_canvas_draw_str(canvas, ROW_LABEL_X, y, labels[i], UI_FONT, color);
        if (i == TX_ROW_BACK) {
            continue; /* у «< BACK» значения нет */
        }
        if (i == selected && editing_numeric) {
            /* Режим правки: короткое число, прокрутка не нужна — рисуем прямо
             * обычным цветом; признак режима — сами стрелки < .. >. */
            gfx_canvas_draw_str(canvas, ROW_VALUE_X, y, edit_buf, UI_FONT, color);
        } else if (i == selected) {
            /* Значение под курсором рисуем через окно: при длинном тексте оно
             * прокручивается, при коротком — просто стоит на месте. */
            update_scroll(values[i]);
            draw_value_clipped(canvas, y, values[i], s_scroll.offset_px, color);
        } else {
            gfx_canvas_draw_str(canvas, ROW_VALUE_X, y, values[i], UI_FONT, color);
        }
    }

    /* Рамка-курсор ОБВОЛАКИВАЕТ текст выбранной строки (метка + значение), а не
     * имеет фиксированную ширину. Если значение не влезло в окно, правый край
     * рамки ограничиваем окном — дальше значение само прокручивается внутри. */
    if (!ui_wifi_has_focus()) {
        int16_t y = row_baseline(selected);
        int16_t right; /* x правого края текста строки на экране */
        if (selected == TX_ROW_BACK) {
            right = ROW_LABEL_X + gfx_canvas_measure_text_width(UI_FONT, "< BACK");
        } else {
            /* В правке рамка охватывает значение со стрелками, иначе — само
             * значение (ограниченное шириной окна, если оно прокручивается). */
            const char *disp = editing_numeric ? edit_buf : values[selected];
            int16_t value_w = gfx_canvas_measure_text_width(UI_FONT, disp);
            if (value_w > ROW_VALUE_VIEW) {
                value_w = ROW_VALUE_VIEW; /* ширина ограничена окном значения */
            }
            right = ROW_VALUE_X + value_w;
        }
        int16_t x = ROW_LABEL_X - CURSOR_PAD;
        int16_t w = (right - ROW_LABEL_X) + CURSOR_PAD * 2;
        int16_t h = TEXT_ASCENT + TEXT_DESCENT + CURSOR_PAD * 2;
        ui_cursor_set_target(cursor, x, y - TEXT_ASCENT - CURSOR_PAD, w, h);
        ui_cursor_step(cursor);
        ui_cursor_draw(canvas, cursor, GFX_WHITE);
    }
}

/* Нужен ли ещё кадр без нажатий кнопок: да, пока крутится длинное значение. */
bool ui_wifi_tx_needs_tick(void)
{
    return s_scroll.active;
}
