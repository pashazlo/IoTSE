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
uint8_t *ui_focus_main_selected(void);
uint8_t *ui_focus_ir_selected(void);
uint8_t *ui_focus_wifi_selected(void);
uint8_t *ui_focus_rf_selected(void);
uint8_t *ui_focus_nrf_selected(void);
uint8_t *ui_focus_bt_selected(void);
uint8_t *ui_focus_settings_selected(void);

#ifdef __cplusplus
}
#endif
