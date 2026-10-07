#pragma once

#include <stdbool.h>

#include "gfx_canvas.h"
#include "ui_cursor.h"
#include "ui_event.h"

void ui_wifi_map_open(void);
void ui_wifi_map_handle_event(ui_event_t event, gfx_canvas_t *canvas);
void ui_wifi_map_poll(gfx_canvas_t *canvas);
void ui_wifi_map_draw(gfx_canvas_t *canvas, ui_cursor_t *cursor);
bool ui_wifi_map_pending(void);
bool ui_wifi_map_needs_tick(void);
