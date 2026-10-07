#pragma once
#include <stdbool.h>
#include "gfx_canvas.h"
#include "ui_event.h"
#include "wifi_port_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_POPUP_NONE = 0,
    UI_POPUP_CANCEL,
    UI_POPUP_RENAME,
    UI_POPUP_DELETE,
    UI_POPUP_EDITOR_NEW_LINE,
    UI_POPUP_EDITOR_BACKSPACE,
    UI_POPUP_EDITOR_SAVE,
    UI_POPUP_EDITOR_SAVE_EXIT,
    UI_POPUP_EDITOR_EXIT_SAVE,
    UI_POPUP_EDITOR_DISCARD,
    UI_POPUP_WIFI_DISCONNECT,
    UI_POPUP_WIFI_AUTO_TOGGLE,
    UI_POPUP_WIFI_FORGET,
    UI_POPUP_WIFI_WEB,
} ui_popup_result_t;

void ui_popup_open(const char *name, bool is_dir);
void ui_popup_open_editor(const char *filename);
void ui_popup_open_editor_exit(const char *filename);
void ui_popup_open_wifi(void);
void ui_popup_open_wifi_network(
    const char *ssid,
    bool saved,
    bool auto_connect
);
void ui_popup_open_wifi_map_port(
    const wifi_probe_result_t *probe,
    const wifi_banner_observation_t *banner
);
void ui_popup_show_message(
    const char *title,
    const char *line1,
    const char *line2
);
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
