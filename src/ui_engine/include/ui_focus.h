#pragma once

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_focus_move(
    uint8_t *selected,
    uint8_t count,
    ui_event_t event
);

#ifdef __cplusplus
}
#endif
