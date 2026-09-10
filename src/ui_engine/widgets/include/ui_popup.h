#pragma once
#include <stdbool.h>
#include "gfx_canvas.h"
#include "ui_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_POPUP_NONE = 0,
    UI_POPUP_CANCEL,
    UI_POPUP_RENAME,
    UI_POPUP_DELETE,
} ui_popup_result_t;

void ui_popup_open(const char *name, bool is_dir);
void ui_popup_close(void);
bool ui_popup_is_open(void);
bool ui_popup_is_animating(void);
/* All UI events must be routed here first while the popup is open. */
ui_popup_result_t ui_popup_handle_event(ui_event_t event);
/* Retain a modal error after a failed operation; X/LEFT/SELECT dismiss it. */
void ui_popup_show_error(const char *message);
void ui_popup_draw(gfx_canvas_t *canvas);

#ifdef __cplusplus
}
#endif
