#pragma once

#include "gfx_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_logo_draw(
    gfx_canvas_t *canvas,
    int16_t cx,
    int16_t cy,
    uint16_t bg_color
);

#ifdef __cplusplus
}
#endif
