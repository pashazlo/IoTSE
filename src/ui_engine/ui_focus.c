#include "ui_focus.h"

#include <math.h>

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


// ============================================================================
// Spatial navigation (задел на будущее — см. комментарий в ui_focus.h)
// ============================================================================

int8_t ui_focus_find_nearest(
    const ui_bbox_t *boxes,
    uint8_t count,
    uint8_t current,
    ui_event_t direction
)
{
    if (boxes == NULL || current >= count) {
        return -1;
    }

    // Центр текущего выбранного объекта — точка отсчёта для поиска.
    float cx = boxes[current].x + boxes[current].w / 2.0f;
    float cy = boxes[current].y + boxes[current].h / 2.0f;

    int8_t best = -1;
    float best_score = 0.0f;

    for (uint8_t i = 0; i < count; i++) {

        if (i == current) {
            continue;
        }

        float ix = boxes[i].x + boxes[i].w / 2.0f;
        float iy = boxes[i].y + boxes[i].h / 2.0f;

        float dx = ix - cx;
        float dy = iy - cy;

        // primary   — расстояние по оси движения (главный критерий)
        // secondary — смещение по перпендикулярной оси (штраф за то,
        //             что объект "не по пути" от текущего к цели)
        bool in_direction = false;
        float primary = 0.0f;
        float secondary = 0.0f;

        switch (direction) {

            case UI_EVT_RIGHT:
                in_direction = dx > 0.5f;
                primary = dx;
                secondary = dy;
                break;

            case UI_EVT_LEFT:
                in_direction = dx < -0.5f;
                primary = -dx;
                secondary = dy;
                break;

            case UI_EVT_DOWN:
                in_direction = dy > 0.5f;
                primary = dy;
                secondary = dx;
                break;

            case UI_EVT_UP:
                in_direction = dy < -0.5f;
                primary = -dy;
                secondary = dx;
                break;

            default:
                break;
        }

        if (!in_direction) {
            // Объект не в ту сторону — не рассматриваем его вообще,
            // даже если он ближайший по прямой линии.
            continue;
        }

        float score = primary + fabsf(secondary) * 2.0f;

        if (best == -1 || score < best_score) {
            best = (int8_t)i;
            best_score = score;
        }
    }

    return best;
}
