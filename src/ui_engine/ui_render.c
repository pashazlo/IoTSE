#include "ui_render.h"

#include "ui_screen.h"
#include "ui_menu.h"
#include "ui_focus.h"
#include "ui_cursor.h"
#include "ui_clock.h"
#include "ui_logo.h"

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


// Ширина строки в пикселях = сумма xAdvance всех глифов.
// Шрифт не строго моноширинный, поэтому "strlen * const" был бы неточным.
static int16_t measure_text_width(const gfx_font_t *font, const char *str)
{
    int16_t w = 0;

    while (*str) {
        uint8_t ch = (uint8_t)*str;
        if (ch >= font->first && ch <= font->last) {
            w += font->glyphs[ch - font->first].xAdvance;
        }
        str++;
    }

    return w;
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
    int16_t text_w = measure_text_width(UI_FONT, text);

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
// Generic Vertical Menu
// ============================================================================

static void draw_vertical_menu(
    gfx_canvas_t *canvas,
    const char *title,
    const char *const *items,
    uint8_t count,
    uint8_t selected
)
{
    gfx_canvas_fill(canvas, 0x0000);

    gfx_canvas_draw_line(
        canvas,
        0,
        18,
        DISPLAY_WIDTH - 1,
        18,
        0xFFFF
    );

    gfx_canvas_draw_str(
        canvas,
        10,
        9,
        title,
        UI_FONT,
        0xFFFF
    );

    const int16_t start_y = 45;
    const int16_t line_h = 20;

    for (uint8_t i = 0; i < count; i++) {

        int16_t y = start_y + (i * line_h);

        if (i == count - 1) {
            y += 15;
        }

        bool focused = (i == selected);

        draw_focus_text(
            canvas,
            10,
            y,
            items[i],
            focused
        );

        if (focused) {
            // Курсор ставим именно на выбранный пункт, после того
            // как он уже нарисован — рамка ляжет поверх текста.
            place_cursor_on_text(canvas, 10, y, items[i]);
        }
    }
}


// ============================================================================
// Main Menu
// ============================================================================

static void draw_main_menu(gfx_canvas_t *canvas)
{
    uint8_t selected = ui_focus_get(UI_FOCUS_MAIN);

    gfx_canvas_fill(canvas, 0x0000);

    ui_clock_draw(canvas);

    gfx_canvas_draw_line(
        canvas,
        0,
        18,
        DISPLAY_WIDTH - 1,
        18,
        0xFFFF
    );

    const int16_t start_y = 40;
    const int16_t line_h = 20;

    for (uint8_t i = 0; i < main_menu_count; i++) {

        int16_t y = start_y + (i * line_h);

        bool focused = (i == selected);

        draw_focus_text(
            canvas,
            10,
            y,
            main_menu[i].title,
            focused
        );

        if (focused) {
            place_cursor_on_text(canvas, 10, y, main_menu[i].title);
        }
    }

    ui_logo_draw(
        canvas,
        220,
        90,
        0x0000
    );
}


// ============================================================================
// IR Menu
// ============================================================================

static void draw_ir_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "IR Remote",
        ir_menu,
        ir_menu_count,
        ui_focus_get(UI_FOCUS_IR)
    );
}


// ============================================================================
// Wi-Fi Menu
// ============================================================================

static void draw_wifi_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "Wi-Fi",
        wifi_menu,
        wifi_menu_count,
        ui_focus_get(UI_FOCUS_WIFI)
    );
}


// ============================================================================
// RF Menu
// ============================================================================

static void draw_rf_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "RF",
        rf_menu,
        rf_menu_count,
        ui_focus_get(UI_FOCUS_RF)
    );
}


// ============================================================================
// NRF24 Menu
// ============================================================================

static void draw_nrf_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "NRF24",
        nrf_menu,
        nrf_menu_count,
        ui_focus_get(UI_FOCUS_NRF)
    );
}


// ============================================================================
// Bluetooth Menu
// ============================================================================

static void draw_bt_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "Bluetooth",
        bt_menu,
        bt_menu_count,
        ui_focus_get(UI_FOCUS_BT)
    );
}


// ============================================================================
// Settings Menu
// ============================================================================

static void draw_settings_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "Settings",
        settings_menu,
        settings_menu_count,
        ui_focus_get(UI_FOCUS_SETTINGS)
    );
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

        case UI_SCREEN_MAIN_MENU:
            draw_main_menu(canvas);
            break;

        case UI_SCREEN_IR_MENU:
            draw_ir_menu(canvas);
            break;

        case UI_SCREEN_RF_MENU:
            draw_rf_menu(canvas);
            break;

        case UI_SCREEN_NRF_MENU:
            draw_nrf_menu(canvas);
            break;

        case UI_SCREEN_WIFI_MENU:
            draw_wifi_menu(canvas);
            break;

        case UI_SCREEN_BT_MENU:
            draw_bt_menu(canvas);
            break;

        case UI_SCREEN_SETTINGS_MENU:
            draw_settings_menu(canvas);
            break;

        default:
            break;
    }

    gfx_canvas_flush(canvas);
}
