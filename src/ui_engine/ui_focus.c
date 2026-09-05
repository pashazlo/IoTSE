#include "ui_focus.h"

// ============================================================================
// Focus State
// ============================================================================

static uint8_t main_selected = 0;
static uint8_t ir_selected = 0;
static uint8_t wifi_selected = 0;
static uint8_t rf_selected = 0;
static uint8_t nrf_selected = 0;
static uint8_t bt_selected = 0;
static uint8_t settings_selected = 0;


// ============================================================================
// Internal State Access
// ============================================================================

static uint8_t *get_focus_state(ui_focus_id_t focus)
{
    switch (focus) {

        case UI_FOCUS_MAIN:
            return &main_selected;

        case UI_FOCUS_IR:
            return &ir_selected;

        case UI_FOCUS_WIFI:
            return &wifi_selected;

        case UI_FOCUS_RF:
            return &rf_selected;

        case UI_FOCUS_NRF:
            return &nrf_selected;

        case UI_FOCUS_BT:
            return &bt_selected;

        case UI_FOCUS_SETTINGS:
            return &settings_selected;

        default:
            return NULL;
    }
}


// ============================================================================
// Focus Movement
// ============================================================================

void ui_focus_move(
    ui_focus_id_t focus,
    uint8_t count,
    ui_event_t event
)
{
    uint8_t *selected = get_focus_state(focus);

    if (selected == NULL || count == 0) {
        return;
    }

    if (*selected >= count) {
        *selected = 0;
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


// ============================================================================
// Get / Set
// ============================================================================

uint8_t ui_focus_get(ui_focus_id_t focus)
{
    uint8_t *selected = get_focus_state(focus);

    if (selected == NULL) {
        return 0;
    }

    return *selected;
}


void ui_focus_set(
    ui_focus_id_t focus,
    uint8_t selected
)
{
    uint8_t *current = get_focus_state(focus);

    if (current == NULL) {
        return;
    }

    *current = selected;
}


// ============================================================================
// Reset
// ============================================================================

void ui_focus_reset(ui_focus_id_t focus)
{
    uint8_t *selected = get_focus_state(focus);

    if (selected == NULL) {
        return;
    }

    *selected = 0;
}


void ui_focus_reset_all(void)
{
    main_selected = 0;
    ir_selected = 0;
    wifi_selected = 0;
    rf_selected = 0;
    nrf_selected = 0;
    bt_selected = 0;
    settings_selected = 0;
}

