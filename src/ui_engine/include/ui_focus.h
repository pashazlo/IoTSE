#pragma once

#include <stdint.h>
#include "ui_event.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Focus IDs
// ============================================================================

typedef enum {
    UI_FOCUS_MAIN = 0,
    UI_FOCUS_IR,
    UI_FOCUS_WIFI,
    UI_FOCUS_RF,
    UI_FOCUS_NRF,
    UI_FOCUS_BT,
    UI_FOCUS_SETTINGS,
    UI_FOCUS_FILE_VOLUMES,
    UI_FOCUS_FILE_BROWSER,
    UI_FOCUS_FILE_EDITOR,
    UI_FOCUS_COUNT   // keep last
} ui_focus_id_t;


// ============================================================================
// Focus API (1D Navigation)
// ============================================================================

/**
 * @brief Move focus according to UI event (1D linear array).
 *
 * UI_EVT_UP   -> previous item
 * UI_EVT_DOWN -> next item
 *
 * Focus wraps around at the beginning/end of the list.
 */
void ui_focus_move(
    ui_focus_id_t focus,
    uint8_t count,
    ui_event_t event
);

/**
 * @brief Get current selected item.
 */
uint8_t ui_focus_get(
    ui_focus_id_t focus
);

/**
 * @brief Set current selected item.
 */
void ui_focus_set(
    ui_focus_id_t focus,
    uint8_t selected
);

/**
 * @brief Reset one focus to the first item.
 */
void ui_focus_reset(
    ui_focus_id_t focus
);

/**
 * @brief Reset all focus positions.
 */
void ui_focus_reset_all(void);


// ============================================================================
// Spatial navigation (2D Grid / Layouts)
// ============================================================================

/**
 * @brief Прямоугольник объекта на экране, в пикселях канвы.
 */
typedef struct {
    int16_t x, y, w, h;
} ui_bbox_t;

/**
 * @brief Найти объект, ближайший к текущему в заданном направлении.
 *
 * @param boxes    массив прямоугольников всех объектов экрана
 * @param count    сколько объектов в массиве
 * @param current  индекс текущего выбранного объекта
 * @param direction UI_EVT_UP / DOWN / LEFT / RIGHT
 *
 * @return индекс ближайшего объекта, либо -1 если в эту сторону объектов нет.
 */
int8_t ui_focus_find_nearest(
    const ui_bbox_t *boxes,
    uint8_t count,
    uint8_t current,
    ui_event_t direction
);

/**
 * @brief Выполнить пространственный сдвиг фокуса (2D-навигация по сетке/виджетам).
 *
 * Если подходящий соседний элемент найден, фокус переключается на него.
 * Если соседей в указанном направлении нет, фокус остаётся на месте.
 */
void ui_focus_move_spatial(
    ui_focus_id_t focus,
    const ui_bbox_t *boxes,
    uint8_t count,
    ui_event_t event
);

#ifdef __cplusplus
}
#endif
