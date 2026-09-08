#include "ui_render.h"

#include "ui_screen.h"
#include "ui_menu.h"
#include "ui_focus.h"
#include "ui_cursor.h"
#include "ui_clock.h"
#include "ui_logo.h"
#include "ui_keyboard.h"
#include <string.h>

#include "fm.h"
#include "fm_text_edit.h"

#include "display.h"
#include "assets/ibm_vga_font.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)


// ============================================================================
// Курсор-рамка (уголки-скобки)
// ============================================================================

// Курсор один на весь UI (а не по одному на каждое меню) — он просто
// "переезжает" между объектами, где бы они ни находились.
static ui_cursor_t s_cursor;

// Отступ рамки-курсора от текста и приблизительные метрики шрифта
// Px437_IBM_VGA_8x14 (высота глифа ~14px: ~11px над базовой линией,
// ~3px под ней). Если поменяете шрифт — возможно, придётся подправить
// эти числа на глаз, т.к. gfx_font_t не хранит общий ascent/descent шрифта.
#define UI_CURSOR_PAD      2
#define UI_TEXT_ASCENT     11
#define UI_TEXT_DESCENT    3


bool ui_render_cursor_is_animating(void)
{
    return s_cursor.animating;
}


// Подвинуть курсор к рамке вокруг текста (text_x/text_y — те же
// координаты, что были переданы в gfx_canvas_draw_str), сделать шаг
// анимации и нарисовать курсор поверх уже отрисованного контента.
// Вызывать РОВНО для того пункта меню, который сейчас выбран.
static void place_cursor_on_text(
    gfx_canvas_t *canvas,
    int16_t text_x,
    int16_t text_y,
    const char *text
)
{
    int16_t text_w = gfx_canvas_measure_text_width(UI_FONT, text);

    int16_t x = text_x - UI_CURSOR_PAD;
    int16_t y = text_y - UI_TEXT_ASCENT - UI_CURSOR_PAD;
    int16_t w = text_w + UI_CURSOR_PAD * 2;
    int16_t h = UI_TEXT_ASCENT + UI_TEXT_DESCENT + UI_CURSOR_PAD * 2;

    ui_cursor_set_target(&s_cursor, x, y, w, h);
    ui_cursor_step(&s_cursor);
    ui_cursor_draw(canvas, &s_cursor, 0xFFFF);
}


// ============================================================================
// Focus Drawing
// ============================================================================

// Раньше выделенный пункт оборачивался в текстовые "[скобки]".
// Теперь выделение показывает курсор-рамка (place_cursor_on_text),
// а здесь остаётся только разный цвет текста — для дополнительной
// наглядности, курсору не мешает.
static void draw_focus_text(
    gfx_canvas_t *canvas,
    int16_t x,
    int16_t y,
    const char *text,
    bool focused
)
{
    gfx_canvas_draw_str(
        canvas,
        x,
        y,
        text,
        UI_FONT,
        focused ? 0xFFFF : 0x8410
    );
}


// ============================================================================
// Splash Screen
// ============================================================================

static void draw_splash_screen(gfx_canvas_t *canvas)
{
    uint16_t background = GFX_RGB565(0xF8, 0x00, 0x54);

    gfx_canvas_fill(canvas, background);

    gfx_canvas_draw_line(
        canvas,
        0,
        12,
        DISPLAY_WIDTH - 1,
        12,
        0xFFFF
    );

    gfx_canvas_draw_line(
        canvas,
        0,
        DISPLAY_HEIGHT - 12,
        DISPLAY_WIDTH - 1,
        DISPLAY_HEIGHT - 12,
        0xFFFF
    );

    ui_logo_draw(
        canvas,
        230,
        DISPLAY_HEIGHT / 2,
        background
    );
}


// ============================================================================
// Menu Screens
// ============================================================================

// Единая отрисовка ЛЮБОГО меню-экрана. Раньше здесь было 8 функций
// (draw_vertical_menu + draw_main_menu + 6 обёрток draw_ir_menu/
// draw_wifi_menu/...) — все почти одинаковые. Теперь одна функция,
// параметризованная данными из ui_menu_get_screen(): что рисовать —
// решает таблица в ui_menu.c, а не отдельная функция здесь.
//
// Главное меню — не совсем "обычное" меню: там часы вместо заголовка
// и логотип в углу. Эти два отличия явно выделены через is_main —
// не отдельная функция ради двух строк разницы.
static void draw_menu_screen(
    gfx_canvas_t *canvas,
    ui_screen_t screen,
    const ui_menu_screen_t *menu
)
{
    bool is_main = (screen == UI_SCREEN_MAIN_MENU);

    gfx_canvas_fill(canvas, 0x0000);

    gfx_canvas_draw_line(
        canvas,
        0,
        18,
        DISPLAY_WIDTH - 1,
        18,
        0xFFFF
    );

    if (is_main) {
        ui_clock_draw(canvas);
    } else if (menu->header) {
        gfx_canvas_draw_str(canvas, 10, 9, menu->header, UI_FONT, 0xFFFF);
    }

    uint8_t selected = ui_focus_get(menu->focus_id);

    const int16_t start_y = is_main ? 40 : 45;
    const int16_t line_h = 20;

    for (uint8_t i = 0; i < menu->count; i++) {

        int16_t y = start_y + (i * line_h);

        // В подменю последний пункт ("< BACK") традиционно отделён
        // зазором от остального списка. У главного меню такого
        // пункта нет — зазор туда не добавляем.
        if (!is_main && i == menu->count - 1) {
            y += 15;
        }

        bool focused = (i == selected);

        draw_focus_text(canvas, 10, y, menu->items[i].title, focused);

        if (focused) {
            // Курсор ставим именно на выбранный пункт, после того
            // как он уже нарисован — рамка ляжет поверх текста.
            place_cursor_on_text(canvas, 10, y, menu->items[i].title);
        }
    }

    if (is_main) {
        ui_logo_draw(canvas, 220, 90, 0x0000);
    }
}


// ============================================================================
// Файловый менеджер — экран выбора тома
// ============================================================================

static void draw_file_volumes_screen(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, 0x0000);
    gfx_canvas_draw_line(canvas, 0, 18, DISPLAY_WIDTH - 1, 18, 0xFFFF);
    gfx_canvas_draw_str(canvas, 10, 9, "Storage", UI_FONT, 0xFFFF);

    uint8_t count = fm_volume_count();
    uint8_t selected = ui_focus_get(UI_FOCUS_FILE_VOLUMES);

    const int16_t start_y = 45;
    const int16_t line_h = 20;

    for (uint8_t i = 0; i < count; i++) {

        const fm_volume_t *vol = fm_get_volume(i);
        if (vol == NULL) {
            continue;
        }

        int16_t y = start_y + (i * line_h);
        bool focused = (i == selected);
        bool available = (vol->is_available == NULL) || vol->is_available();

        // Недоступный том (например, SD не вставлена) рисуем тусклым,
        // независимо от фокуса — чтобы сразу было видно, что выбирать
        // его сейчас бесполезно.
        uint16_t color = !available ? 0x4208 : (focused ? 0xFFFF : 0x8410);

        gfx_canvas_draw_str(canvas, 10, y, vol->label, UI_FONT, color);

        if (focused && available) {
            place_cursor_on_text(canvas, 10, y, vol->label);
        }
    }
}


// ============================================================================
// Файловый менеджер — браузер файлов текущей директории
// ============================================================================

static void draw_file_browser_screen(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, 0x0000);
    gfx_canvas_draw_line(canvas, 0, 18, DISPLAY_WIDTH - 1, 18, 0xFFFF);

    // Заголовок — текущий путь. Может быть длиннее экрана при глубокой
    // вложенности, но это диагностическая строка, не главный контент —
    // обрезание длинных путей оставляем на будущее по необходимости.
    gfx_canvas_draw_str(canvas, 10, 9, fm_current_path(), UI_FONT, 0xFFFF);

    uint8_t real_count = fm_get_cached_count();
    uint8_t selected = ui_focus_get(UI_FOCUS_FILE_BROWSER);

    const int16_t start_y = 32;
    const int16_t line_h = 18;

    // Синтетические пункты "создать" — всегда первыми в списке.
    bool sel_new_folder = (selected == 0);
    bool sel_new_file = (selected == 1);

    int16_t y0 = start_y;
    draw_focus_text(canvas, 10, y0, "[+ New Folder]", sel_new_folder);
    if (sel_new_folder) {
        place_cursor_on_text(canvas, 10, y0, "[+ New Folder]");
    }

    int16_t y1 = start_y + line_h;
    draw_focus_text(canvas, 10, y1, "[+ New File]", sel_new_file);
    if (sel_new_file) {
        place_cursor_on_text(canvas, 10, y1, "[+ New File]");
    }

    for (uint8_t i = 0; i < real_count; i++) {

        const fm_entry_t *entry = fm_get_cached_entry(i);
        if (entry == NULL) {
            continue;
        }

        int16_t y = start_y + ((i + 2) * line_h);
        bool focused = (selected == i + 2);

        // Папки помечаем "/" в конце имени — простое, но однозначное
        // визуальное отличие без отдельной иконки.
        char label[FM_MAX_NAME_LEN + 2];
        if (entry->is_dir) {
            snprintf(label, sizeof(label), "%s/", entry->name);
        } else {
            strncpy(label, entry->name, sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
        }

        draw_focus_text(canvas, 10, y, label, focused);

        if (focused) {
            place_cursor_on_text(canvas, 10, y, label);
        }
    }

    if (real_count == 0) {
        gfx_canvas_draw_str(canvas, 10, start_y + 2 * line_h, "(empty)", UI_FONT, 0x8410);
    }
}


// ============================================================================
// Файловый менеджер — построчный редактор текста
// ============================================================================

static void draw_file_editor_screen(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, 0x0000);
    gfx_canvas_draw_line(canvas, 0, 18, DISPLAY_WIDTH - 1, 18, 0xFFFF);
    gfx_canvas_draw_str(canvas, 10, 9, "Editor  (LEFT=save & exit)", UI_FONT, 0xFFFF);

    uint16_t count = fm_text_edit_line_count();
    uint8_t selected = ui_focus_get(UI_FOCUS_FILE_EDITOR);

    const int16_t start_y = 32;
    const int16_t line_h = 16;

    // Сколько строк вообще помещается на экран — простая постраничная
    // прокрутка "вокруг выбранной строки", без отдельного индикатора.
    uint8_t visible_lines = (DISPLAY_HEIGHT - start_y) / line_h;

    uint16_t scroll_top = 0;
    if (selected >= visible_lines) {
        scroll_top = selected - visible_lines + 1;
    }

    for (uint8_t row = 0; row < visible_lines; row++) {

        uint16_t idx = scroll_top + row;
        if (idx >= count) {
            break;
        }

        int16_t y = start_y + (row * line_h);
        bool focused = (idx == selected);

        draw_focus_text(canvas, 10, y, fm_text_edit_get_line(idx), focused);

        if (focused) {
            place_cursor_on_text(canvas, 10, y, fm_text_edit_get_line(idx));
        }
    }
}


// ============================================================================
// Main Render
// ============================================================================

void ui_render(gfx_canvas_t *canvas)
{
    // При переходе на другой экран курсор не должен "лететь" через
    // весь дисплей с прошлого места (например, из главного меню
    // в подменю Wi-Fi) — он просто появляется заново на новом месте.
    static ui_screen_t s_prev_screen;
    static bool s_first_call = true;

    ui_screen_t screen = ui_screen_get();

    if (s_first_call || screen != s_prev_screen) {
        ui_cursor_reset(&s_cursor);
        s_prev_screen = screen;
        s_first_call = false;
    }

    switch (screen) {

        case UI_SCREEN_SPLASH:
            draw_splash_screen(canvas);
            break;

        case UI_SCREEN_FILE_VOLUMES:
            draw_file_volumes_screen(canvas);
            break;

        case UI_SCREEN_FILE_BROWSER:
            draw_file_browser_screen(canvas);
            break;

        case UI_SCREEN_FILE_EDITOR:
            draw_file_editor_screen(canvas);
            break;

        default: {
            const ui_menu_screen_t *menu = ui_menu_get_screen(screen);
            if (menu != NULL) {
                draw_menu_screen(canvas, screen, menu);
            }
            break;
        }
    }

    // Клавиатура — модальная поверх ВСЕГО, что нарисовано выше.
    // Рисуется последней, и только когда реально открыта.
    if (ui_keyboard_is_open()) {
        ui_keyboard_draw(canvas);
    }

    gfx_canvas_flush(canvas);
}
