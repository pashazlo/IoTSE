#pragma once

#include "gfx_canvas.h"
#include "ui_cursor.h"
#include "ui_event.h"

void ui_wifi_export_open(void);
void ui_wifi_export_handle_event(ui_event_t event, gfx_canvas_t *canvas);
void ui_wifi_export_draw(gfx_canvas_t *canvas, ui_cursor_t *cursor);
bool ui_wifi_export_needs_tick(void);
