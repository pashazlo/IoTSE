#pragma once

#include <stdbool.h>
#include "gfx_canvas.h"
#include "ui_event.h"
#include "ui_cursor.h"

void ui_wifi_draw(gfx_canvas_t *canvas, ui_cursor_t *shared_cursor);
bool ui_wifi_handle_event(ui_event_t event);
bool ui_wifi_is_animating(void);
bool ui_wifi_details_is_open(void);
bool ui_wifi_has_focus(void);
void ui_wifi_request_disconnect(void);
