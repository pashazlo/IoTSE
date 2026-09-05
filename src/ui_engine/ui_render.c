
#include "ui_render.h"

#include "ui_screen.h"
#include "ui_menu.h"
#include "ui_focus.h"
#include "ui_clock.h"
#include "ui_logo.h"

#include "display.h"
#include "assets/ibm_vga_font.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b7a)


static void draw_focus_text(
    gfx_canvas_t *canvas,
    int16_t x,
    int16_t y,
    const char *text,
    bool focused
)
{
    if (focused) {
        char buffer[64];

        snprintf(
            buffer,
            sizeof(buffer),
            "[%s]",
            text
        );

        gfx_canvas_draw_str(
            canvas,
            x,
            y,
            buffer,
            UI_FONT,
            0xFFFF
        );
    } else {
        gfx_canvas_draw_str(
            canvas,
            x,
            y,
            text,
            UI_FONT,
            0x8410
        );
    }
}


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

        draw_focus_text(
            canvas,
            10,
            y,
            items[i],
            i == selected
        );
    }
}


static void draw_main_menu(gfx_canvas_t *canvas)
{
    uint8_t *selected = ui_focus_main_selected();

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

        draw_focus_text(
            canvas,
            10,
            y,
            main_menu[i].title,
            i == *selected
        );
    }

    ui_logo_draw(
        canvas,
        220,
        90,
        0x0000
    );
}


static void draw_ir_menu(gfx_canvas_t *canvas)
{
    uint8_t *selected = ui_focus_ir_selected();

    draw_vertical_menu(
        canvas,
        "IR Remote",
        ir_menu,
        ir_menu_count,
        *selected
    );
}


static void draw_wifi_menu(gfx_canvas_t *canvas)
{
    uint8_t *selected = ui_focus_wifi_selected();

    draw_vertical_menu(
        canvas,
        "Wi-Fi",
        wifi_menu,
        wifi_menu_count,
        *selected
    );
}


static void draw_rf_menu(gfx_canvas_t *canvas)
{
    uint8_t *selected = ui_focus_rf_selected();

    draw_vertical_menu(
        canvas,
        "RF",
        rf_menu,
        rf_menu_count,
        *selected
    );
}


static void draw_nrf_menu(gfx_canvas_t *canvas)
{
    uint8_t *selected = ui_focus_nrf_selected();

    draw_vertical_menu(
        canvas,
        "NRF24",
        nrf_menu,
        nrf_menu_count,
        *selected
    );
}


static void draw_bt_menu(gfx_canvas_t *canvas)
{
    uint8_t *selected = ui_focus_bt_selected();

    draw_vertical_menu(
        canvas,
        "Bluetooth",
        bt_menu,
        bt_menu_count,
        *selected
    );
}


static void draw_settings_menu(gfx_canvas_t *canvas)
{
    uint8_t *selected = ui_focus_settings_selected();

    draw_vertical_menu(
        canvas,
        "Settings",
        settings_menu,
        settings_menu_count,
        *selected
    );
}


void ui_render(gfx_canvas_t *canvas)
{
    switch (ui_screen_get()) {

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

