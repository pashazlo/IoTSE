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
    UI_FOCUS_COUNT   // keep last
} ui_focus_id_t;


// ============================================================================
// Focus API
// ============================================================================

/**
 * @brief Move focus according to UI event.
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
// Spatial navigation (задел на будущее — для виджетов, не список)
// ============================================================================

/**
 * @brief Прямоугольник объекта на экране, в пикселях канвы.
 *
 * Нужен для ui_focus_find_nearest() ниже. Сейчас вертикальные меню
 * навигируются простым "selected +-1 с зацикливанием" через
 * ui_focus_move() — этого достаточно, пока объекты в один столбец.
 *
 * Когда появятся экраны с произвольным расположением виджетов
 * (не список сверху вниз, а сетка/дашборд), ЛЕВО/ПРАВО там уже не
 * получится свести к "+-1" — вот тогда и пригодится bbox + функция
 * ниже.
 */
typedef struct {
    int16_t x, y, w, h;
} ui_bbox_t;

/**
 * @brief Найти объект, ближайший к текущему в заданном направлении.
 *
 * Алгоритм: среди всех объектов, чей центр лежит СТРОГО в нужную
 * сторону от центра текущего объекта, выбирается тот, у которого
 * минимален score = (расстояние по оси движения) + 2 * (смещение
 * по перпендикулярной оси). То есть побеждает не просто ближайший
 * по прямой, а ближайший "по пути" в нужном направлении — иначе
 * объект чуть в стороне мог бы перебить объект прямо по курсу.
 *
 * @param boxes    массив прямоугольников всех объектов экрана
 * @param count    сколько объектов в массиве
 * @param current  индекс текущего выбранного объекта
 * @param direction UI_EVT_UP / DOWN / LEFT / RIGHT (другие значения — no-op)
 *
 * @return индекс ближайшего подходящего объекта, либо -1, если в эту
 *         сторону подходящих объектов нет (значит, фокус остаётся
 *         на месте — типичное поведение spatial-навигации, в отличие
 *         от зацикливания в ui_focus_move()).
 */
int8_t ui_focus_find_nearest(
    const ui_bbox_t *boxes,
    uint8_t count,
    uint8_t current,
    ui_event_t direction
);

#ifdef __cplusplus
}
#endif
