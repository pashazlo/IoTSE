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
