#include "ui_focus.h"
#include <string.h>
#include <stdlib.h>

// ============================================================================
// Focus State
// ============================================================================

// Массив состояний фокуса для всех экранов
static uint8_t s_selected[UI_FOCUS_COUNT] = {0};

// ============================================================================
// Focus Movement (Линейный список: Вверх / Вниз)
// ============================================================================

void ui_focus_move(ui_focus_id_t focus, uint8_t count, ui_event_t event)
{
    if (focus >= UI_FOCUS_COUNT || count == 0) {
        return;
    }

    uint8_t *selected = &s_selected[focus];

    // Защита: если список уменьшился, сбрасываем фокус на последний элемент
    if (*selected >= count) {
        *selected = count - 1;
    }

    if (event == UI_EVT_UP) {
        if (*selected == 0) {
            *selected = count - 1; // Зацикливание вверх
        } else {
            (*selected)--;
        }
    } else if (event == UI_EVT_DOWN) {
        if (*selected >= count - 1) {
            *selected = 0; // Зацикливание вниз
        } else {
            (*selected)++;
        }
    }
}

// ============================================================================
// Get / Set / Reset
// ============================================================================

uint8_t ui_focus_get(ui_focus_id_t focus)
{
    if (focus >= UI_FOCUS_COUNT) {
        return 0;
    }

    return s_selected[focus];
}

void ui_focus_set(ui_focus_id_t focus, uint8_t selected)
{
    if (focus >= UI_FOCUS_COUNT) {
        return;
    }

    s_selected[focus] = selected;
}

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
// Spatial Navigation (2D-сетки, иконки, произвольное меню)
// ============================================================================

int8_t ui_focus_find_nearest(
    const ui_bbox_t *boxes,
    uint8_t count,
    uint8_t current,
    ui_event_t direction
)
{
    if (boxes == NULL || current >= count || count == 0) {
        return -1;
    }

    // Вычисляем центр текущего элемента (целочисленная математика для скорости)
    int32_t cx = boxes[current].x + (boxes[current].w / 2);
    int32_t cy = boxes[current].y + (boxes[current].h / 2);

    int8_t best_index = -1;
    int32_t best_score = INT32_MAX;

    for (uint8_t i = 0; i < count; i++) {
        if (i == current) {
            continue;
        }

        int32_t ix = boxes[i].x + (boxes[i].w / 2);
        int32_t iy = boxes[i].y + (boxes[i].h / 2);

        int32_t dx = ix - cx;
        int32_t dy = iy - cy;

        bool in_direction = false;
        int32_t primary = 0;
        int32_t secondary = 0;

        switch (direction) {
            case UI_EVT_RIGHT:
                in_direction = (dx > 0);
                primary = dx;
                secondary = dy;
                break;

            case UI_EVT_LEFT:
                in_direction = (dx < 0);
                primary = -dx;
                secondary = dy;
                break;

            case UI_EVT_DOWN:
                in_direction = (dy > 0);
                primary = dy;
                secondary = dx;
                break;

            case UI_EVT_UP:
                in_direction = (dy < 0);
                primary = -dy;
                secondary = dx;
                break;

            default:
                break;
        }

        if (!in_direction) {
            continue;
        }

        // Метрика штрафа: расстояние по оси + двойной штраф за отклонение от оси
        int32_t score = primary + (abs(secondary) * 2);

        if (score < best_score) {
            best_score = score;
            best_index = (int8_t)i;
        }
    }

    return best_index;
}

// Удобная обертка для 2D-перемещения по Bounding Box
void ui_focus_move_spatial(
    ui_focus_id_t focus,
    const ui_bbox_t *boxes,
    uint8_t count,
    ui_event_t event
)
{
    if (focus >= UI_FOCUS_COUNT || boxes == NULL || count == 0) {
        return;
    }

    uint8_t current = ui_focus_get(focus);
    
    // Авто-коррекция, если фокус за границами
    if (current >= count) {
        current = 0;
        ui_focus_set(focus, current);
    }

    int8_t next = ui_focus_find_nearest(boxes, count, current, event);
    if (next >= 0) {
        ui_focus_set(focus, (uint8_t)next);
    }
}
