#include "ui_focus.h"

static uint8_t main_selected = 0;
static uint8_t ir_selected = 0;
static uint8_t wifi_selected = 0;
static uint8_t rf_selected = 0;
static uint8_t nrf_selected = 0;
static uint8_t bt_selected = 0;
static uint8_t settings_selected = 0;

void ui_focus_move(
    uint8_t *selected,
    uint8_t count,
    ui_event_t event
)
{
    if (selected == NULL || count == 0) {
        return;
    }

    if (event == UI_EVT_UP) {

        if (*selected == 0) {
            *selected = count - 1;
        } else {
            (*selected)--;
        }
    }

    else if (event == UI_EVT_DOWN) {

        if (*selected >= count - 1) {
            *selected = 0;
        } else {
            (*selected)++;
        }
    }
}
uint8_t *ui_focus_main_selected(void)
{
    return &main_selected;
}

uint8_t *ui_focus_ir_selected(void)
{
    return &ir_selected;
}

uint8_t *ui_focus_wifi_selected(void)
{
    return &wifi_selected;
}

uint8_t *ui_focus_rf_selected(void)
{
    return &rf_selected;
}

uint8_t *ui_focus_nrf_selected(void)
{
    return &nrf_selected;
}

uint8_t *ui_focus_bt_selected(void)
{
    return &bt_selected;
}

uint8_t *ui_focus_settings_selected(void)
{
    return &settings_selected;
}
