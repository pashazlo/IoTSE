#pragma once

#include "gfx_canvas.h"
#include "ui_event.h"
#include "wifi_worker.h"

void ui_wifi_monitor_open(void);
void ui_wifi_monitor_handle_event(ui_event_t event, gfx_canvas_t *canvas);
void ui_wifi_monitor_draw(gfx_canvas_t *canvas);
void ui_wifi_monitor_poll(gfx_canvas_t *canvas);
bool ui_wifi_monitor_handle_worker_result(const wifi_worker_result_t *result);
bool ui_wifi_monitor_cursor_is_animating(void);
