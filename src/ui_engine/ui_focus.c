#include "ui_focus.h"

#include <math.h>
#include <string.h>

// ============================================================================
// Focus State
// ============================================================================

// Один массив на все экраны, индексируется прямо значением enum
// (UI_FOCUS_MAIN, UI_FOCUS_IR, ...). При добавлении нового
// UI_FOCUS_XYZ в ui_focus.h массив автоматически станет на один
// элемент больше — здесь ничего дописывать не нужно.
static uint8_t s_selected[UI_FOCUS_COUNT];


// ============================================================================
// Focus Movement
// ============================================================================

void ui_focus_move(
    ui_focus_id_t focus,
    uint8_t count,
    ui_event_t event
)
{
    if (focus >= UI_FOCUS_COUNT || count == 0) {
        return;
    }

    uint8_t *selected = &s_selected[focus];

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
    if (focus >= UI_FOCUS_COUNT) {
        return 0;
    }

    return s_selected[focus];
}


void ui_focus_set(
    ui_focus_id_t focus,
    uint8_t selected
)
{
    if (focus >= UI_FOCUS_COUNT) {
        return;
    }

    s_selected[focus] = selected;
}


// ============================================================================
// Reset
// ============================================================================

void ui_focus_reset(ui_focus_id_t focus)
{
    if (focus >= UI_FOCUS_COUNT) {
        return;
    }

    s_selected[focus] = 0;
}


void ui_focus_reset_all(void)
{
    memset(s_selected, 0, sizeof(s_selected));
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
