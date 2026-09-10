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
    UI_EVT_CONTEXT, // SELECT held for 700 ms; release does not emit SELECT.
} ui_event_t;

#ifdef __cplusplus
}
#endif
