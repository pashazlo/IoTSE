#include "ui_render.h"

#include "ui_screen.h"
#include "ui_menu.h"

#include "display.h"
#include "assets/ibm_vga_font.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b7a)

static void draw_redteam_logo(
    gfx_canvas_t *canvas,
    int16_t cx,
    int16_t cy,
    uint16_t bg_color
)
{
    gfx_canvas_draw_circle(canvas, cx, cy, 42, 0xFFFF);
    gfx_canvas_draw_circle(canvas, cx, cy, 41, 0xFFFF);
    gfx_canvas_draw_circle(canvas, cx, cy, 26, 0xFFFF);
    gfx_canvas_fill_circle(canvas, cx, cy, 11, 0xFFFF);

    gfx_canvas_fill_circle(
        canvas,
        cx - 5,
        cy - 5,
        3,
        bg_color
    );

    const int16_t arm = 68;
    const int16_t gap = 46;
    const int16_t tick = 4;

    gfx_canvas_draw_line(canvas, cx - arm, cy, cx - gap, cy, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx + gap, cy, cx + arm, cy, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx, cy - arm, cx, cy - gap, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx, cy + gap, cx, cy + arm, 0xFFFF);

    gfx_canvas_draw_line(canvas, cx - arm, cy - tick, cx - arm, cy + tick, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx + arm, cy - tick, cx + arm, cy + tick, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx - tick, cy - arm, cx + tick, cy - arm, 0xFFFF);
    gfx_canvas_draw_line(canvas, cx - tick, cy + arm, cx + tick, cy + arm, 0xFFFF);
}

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

        snprintf(buffer, sizeof(buffer), "[%s]", text);

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

    draw_redteam_logo(
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
    int count,
    int8_t selected
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

    for (int i = 0; i < count; i++) {
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

static void draw_main_clock(gfx_canvas_t *canvas)
{
    char time_buffer[16];

    time_t now;
    struct tm time_info;

    time(&now);
    localtime_r(&now, &time_info);

    strftime(
        time_buffer,
        sizeof(time_buffer),
        "%H:%M",
        &time_info
    );

    gfx_canvas_draw_str(
        canvas,
        140,
        14,
        time_buffer,
        UI_FONT,
        0xFFFF
    );
}

static void draw_main_menu(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, 0x0000);

    draw_main_clock(canvas);

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

    for (int i = 0; i < MENU_COUNT; i++) {
        int16_t y = start_y + (i * line_h);

        draw_focus_text(
            canvas,
            10,
            y,
            main_menu[i].title,
            false
        );
    }

    draw_redteam_logo(
        canvas,
        220,
        90,
        0x0000
    );
}

static void draw_ir_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "IR Remote",
        ir_menu,
        IR_MENU_COUNT,
        0
    );
}

static void draw_wifi_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "Wi-Fi",
        wifi_menu,
        WIFI_MENU_COUNT,
        0
    );
}

static void draw_rf_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "RF",
        rf_menu,
        RF_MENU_COUNT,
        0
    );
}

static void draw_nrf_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "NRF24",
        nrf_menu,
        NRF_MENU_COUNT,
        0
    );
}

static void draw_bt_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "Bluetooth",
        bt_menu,
        BLUETOOTH_MENU_COUNT,
        0
    );
}

static void draw_settings_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "Settings",
        settings_menu,
        SETTINGS_MENU_COUNT,
        0
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
