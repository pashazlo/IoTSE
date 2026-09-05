#include "ui_logo.h"

#include <stdint.h>

void ui_logo_draw(
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

    gfx_canvas_draw_line(
        canvas,
        cx - arm, cy,
        cx - gap, cy,
        0xFFFF
    );

    gfx_canvas_draw_line(
        canvas,
        cx + gap, cy,
        cx + arm, cy,
        0xFFFF
    );

    gfx_canvas_draw_line(
        canvas,
        cx, cy - arm,
        cx, cy - gap,
        0xFFFF
    );

    gfx_canvas_draw_line(
        canvas,
        cx, cy + gap,
        cx, cy + arm,
        0xFFFF
    );

    gfx_canvas_draw_line(
        canvas,
        cx - arm, cy - tick,
        cx - arm, cy + tick,
        0xFFFF
    );

    gfx_canvas_draw_line(
        canvas,
        cx + arm, cy - tick,
        cx + arm, cy + tick,
        0xFFFF
    );

    gfx_canvas_draw_line(
        canvas,
        cx - tick, cy - arm,
        cx + tick, cy - arm,
        0xFFFF
    );

    gfx_canvas_draw_line(
        canvas,
        cx - tick, cy + arm,
        cx + tick, cy + arm,
        0xFFFF
    );
}
