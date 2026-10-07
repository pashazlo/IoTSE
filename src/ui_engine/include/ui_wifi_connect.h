#pragma once

#include <stdbool.h>

#include "gfx_canvas.h"
#include "ui_cursor.h"
#include "ui_event.h"
#include "ui_popup.h"

void ui_wifi_connect_begin(void);
void ui_wifi_connect_handle_event(ui_event_t event, gfx_canvas_t *canvas);
void ui_wifi_connect_handle_keyboard_result(void);
bool ui_wifi_connect_handle_popup_result(ui_popup_result_t result);
void ui_wifi_connect_poll(gfx_canvas_t *canvas);
void ui_wifi_connect_draw(gfx_canvas_t *canvas, ui_cursor_t *cursor);
bool ui_wifi_connect_pending(void);
bool ui_wifi_connect_needs_tick(void);
