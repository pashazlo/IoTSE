#include "ui_controller.h"

#include "ui_screen.h"
#include "ui_menu.h"
#include "ui_focus.h"
#include "ui_render.h"

// ============================================================================
// Единый обработчик событий для ВСЕХ меню-экранов.
//
// Раньше здесь было 6 почти одинаковых функций (main/ir/wifi/rf/nrf/bt/
// settings) — отличались только тем, какой ui_focus_id_t и какой массив
// пунктов использовать. Теперь эта разница вынесена в таблицу
// ui_menu_get_screen() (см. ui_menu.c), а сама логика обработки
// UP/DOWN/SELECT одна на всех.
//
// Добавление нового меню-экрана НЕ требует правок в этом файле —
// только новая запись в таблице в ui_menu.c.
// ============================================================================

void ui_controller_handle_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    const ui_menu_screen_t *menu = ui_menu_get_screen(ui_screen_get());

    // NULL — значит текущий экран не меню (сплэш) либо ещё не описан
    // в таблице. В обоих случаях кнопки здесь обрабатывать нечем.
    if (menu == NULL) {
        return;
    }

    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            ui_focus_move(menu->focus_id, menu->count, evt);
            ui_render(canvas);
            break;

        case UI_EVT_SELECT: {

            uint8_t selected = ui_focus_get(menu->focus_id);

            if (menu->items[selected].callback) {
                menu->items[selected].callback();
            }

            ui_render(canvas);
            break;
        }

        case UI_EVT_LEFT:
        case UI_EVT_RIGHT:
            // Задел на будущее: когда на экране появятся объекты не
            // в один столбец (сетка виджетов), здесь будет вызов
            // ui_focus_find_nearest() вместо ui_focus_move(). Пока
            // все меню — вертикальный список, двигать здесь нечего.
            break;

        default:
            break;
    }
}
