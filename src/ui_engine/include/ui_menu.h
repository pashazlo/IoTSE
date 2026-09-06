#pragma once

#include <stdint.h>

#include "ui_event.h"
#include "ui_focus.h"
#include "ui_screen.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Один пункт меню
// ============================================================================

typedef void (*menu_cb_t)(void);

typedef struct {
    const char *title;
    menu_cb_t callback;   // NULL = пункт-заглушка, при SELECT ничего не делает
} menu_item_t;


// ============================================================================
// Описание меню-экрана целиком
// ============================================================================

/**
 * @brief Всё, что нужно controller'у и render'у, чтобы отработать
 *        один экран-меню, — в одной структуре. Раньше под каждый
 *        экран были отдельная функция в ui_controller.c и отдельная
 *        функция в ui_render.c; теперь обе используют общий код,
 *        параметризованный этой структурой (см. ui_menu_get_screen).
 */
typedef struct {
    ui_focus_id_t focus_id;      // куда ui_focus.c пишет выбранный индекс
    const char *header;          // текст заголовка; NULL — у главного меню
                                  // (там вместо заголовка рисуются часы)
    const menu_item_t *items;
    uint8_t count;
} ui_menu_screen_t;

/**
 * @brief Получить описание меню для данного экрана.
 *
 * @return указатель на статические (в ui_menu.c) данные меню, либо
 *         NULL для UI_SCREEN_SPLASH или неизвестного значения —
 *         там меню в принципе нет, и controller/render должны
 *         просто ничего не делать в ветке меню.
 */
const ui_menu_screen_t *ui_menu_get_screen(ui_screen_t screen);

#ifdef __cplusplus
}
#endif
