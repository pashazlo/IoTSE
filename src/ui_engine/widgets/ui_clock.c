#include "ui_clock.h"

#include "display.h"
#include "assets/ibm_vga_font.h"

#include <stddef.h>
#include <time.h>

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)

void ui_clock_draw(gfx_canvas_t *canvas)
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
