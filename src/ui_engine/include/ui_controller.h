#pragma once

#include "ui_event.h"
#include "gfx_canvas.h"

void ui_controller_handle_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
);
