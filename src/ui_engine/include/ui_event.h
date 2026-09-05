// ui_event.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_EVT_NONE = 0,
    UI_EVT_UP,
    UI_EVT_DOWN,
    UI_EVT_LEFT,
    UI_EVT_RIGHT,
    UI_EVT_SELECT,
} ui_event_t;

#ifdef __cplusplus
}
#endif
