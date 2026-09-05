#include "ui.h"

#include "display.h"
#include "gfx_canvas.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"

// ============================================================================
// Configuration
// ============================================================================

static const char *TAG = "UI";

#define UI_QUEUE_LEN 10

// ============================================================================
// UI State
// ============================================================================

// Очередь событий интерфейса.
// Сюда input_bridge отправляет UI_EVT_UP, UI_EVT_DOWN и другие события.
static QueueHandle_t ui_queue = NULL;




// ============================================================================
// Menu Types
// ============================================================================

// ============================================================================
// Screen Actions
// ============================================================================

// Пока callbacks нужны только главному меню.
// Они переключают current_screen на соответствующий экран.


// ============================================================================
// System Time
// ============================================================================


// ============================================================================
// Menu Data
// ============================================================================

// Главное меню устройства.



// ============================================================================
// Menu Counts
// ============================================================================

// Автоматически считаем количество элементов массива.
// Теперь не нужно вручную помнить, сколько пунктов в меню.


// ============================================================================
// Focus State
// ============================================================================

// Каждый экран хранит собственный текущий выбранный элемент.
//
// Это важно:
// если ты вышел из IR и потом вернулся,
// IR помнит, где последний раз находился фокус.


// ============================================================================
// Red Team Logo
// ============================================================================


// ============================================================================
// Focus Drawing
// ============================================================================

// Базовая функция первого уровня системы фокуса.
//
// focused = true:
//      [TEXT] белым цветом
//
// focused = false:
//      TEXT серым цветом
//
// Позже именно эту функцию можно расширить:
// добавить рамки, иконки, анимацию, курсор и другие типы объектов.


// ============================================================================
// Focus Movement
// ============================================================================

// Первый уровень системы навигации.
//
// Пока реализована вертикальная навигация:
//
// UP   -> предыдущий элемент
// DOWN -> следующий элемент
//
// При выходе за границы меню происходит переход по кругу.

// ============================================================================
// Splash Screen
// ============================================================================


// ============================================================================
// Generic Menu Drawing
// ============================================================================

// Универсальная отрисовка вертикального меню.
//
// title      - заголовок экрана
// items      - массив пунктов меню
// count      - количество пунктов
// selected   - текущий элемент в фокусе


// ============================================================================
// Main Screen Clock
// ============================================================================


// ============================================================================
// Main Menu
// ============================================================================



// ============================================================================
// IR Menu
// ============================================================================



// ============================================================================
// Wi-Fi Menu
// ============================================================================



// ============================================================================
// RF Menu
// ============================================================================


// ============================================================================
// NRF24 Menu
// ============================================================================


// ============================================================================
// Bluetooth Menu
// ============================================================================


// ============================================================================
// Settings Menu
// ============================================================================



// ============================================================================
// Render
// ============================================================================

// Единственная точка полной отрисовки интерфейса.
//
// Сначала определяется current_screen,
// затем вызывается функция отрисовки конкретного экрана,
// после чего canvas отправляется на дисплей.



// ============================================================================
// Main Menu Event Handler
// ============================================================================

static void handle_main_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &main_selected,
                MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (main_menu[main_selected].callback) {

                main_menu[main_selected].callback();

                ui_render(canvas);
            }

            break;


        // LEFT / RIGHT пока зарезервированы под пространственный фокус.
        case UI_EVT_LEFT:
        case UI_EVT_RIGHT:

            ESP_LOGI(TAG, "Main menu horizontal navigation");

            break;


        default:
            break;
    }
}


// ============================================================================
// IR Event Handler
// ============================================================================

static void handle_ir_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &ir_selected,
                IR_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            // Последний пункт любого меню — BACK.
            if (ir_selected == IR_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        case UI_EVT_LEFT:
        case UI_EVT_RIGHT:

            ESP_LOGI(TAG, "IR horizontal navigation");

            break;


        default:
            break;
    }
}


// ============================================================================
// Wi-Fi Event Handler
// ============================================================================

static void handle_wifi_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &wifi_selected,
                WIFI_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (wifi_selected == WIFI_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        case UI_EVT_LEFT:
        case UI_EVT_RIGHT:

            ESP_LOGI(TAG, "Wi-Fi horizontal navigation");

            break;


        default:
            break;
    }
}


// ============================================================================
// RF Event Handler
// ============================================================================

static void handle_rf_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &rf_selected,
                RF_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (rf_selected == RF_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        default:
            break;
    }
}


// ============================================================================
// NRF24 Event Handler
// ============================================================================

static void handle_nrf_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &nrf_selected,
                NRF_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (nrf_selected == NRF_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        default:
            break;
    }
}


// ============================================================================
// Bluetooth Event Handler
// ============================================================================

static void handle_bt_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &bt_selected,
                BLUETOOTH_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (bt_selected == BLUETOOTH_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        default:
            break;
    }
}


// ============================================================================
// Settings Event Handler
// ============================================================================

static void handle_settings_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &settings_selected,
                SETTINGS_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (settings_selected == SETTINGS_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        default:
            break;
    }
}


// ============================================================================
// UI Event Router
// ============================================================================

// Центральный маршрутизатор событий.
//
// Здесь определяется:
// какой экран сейчас активен
// и какому обработчику передать событие.

static void ui_handle_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (current_screen) {

        case UI_SCREEN_MAIN_MENU:
            handle_main_menu_event(evt, canvas);
            break;

        case UI_SCREEN_IR_MENU:
            handle_ir_menu_event(evt, canvas);
            break;

        case UI_SCREEN_RF_MENU:
            handle_rf_menu_event(evt, canvas);
            break;

        case UI_SCREEN_NRF_MENU:
            handle_nrf_menu_event(evt, canvas);
            break;

        case UI_SCREEN_WIFI_MENU:
            handle_wifi_menu_event(evt, canvas);
            break;

        case UI_SCREEN_BT_MENU:
            handle_bt_menu_event(evt, canvas);
            break;

        case UI_SCREEN_SETTINGS_MENU:
            handle_settings_menu_event(evt, canvas);
            break;

        default:
            break;
    }
}


// ============================================================================
// UI Initialization
// ============================================================================

esp_err_t ui_init(void)
{
    if (ui_queue != NULL) {

        ESP_LOGW(TAG, "UI already initialized");

        return ESP_OK;
    }

    ui_queue = xQueueCreate(
        UI_QUEUE_LEN,
        sizeof(ui_event_t)
    );

    if (ui_queue == NULL) {

        ESP_LOGE(TAG, "Failed to create UI queue");

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "UI queue created");

    return ESP_OK;
}


// ============================================================================
// Event Sending API
// ============================================================================

BaseType_t ui_send_event(ui_event_t evt)
{
    if (ui_queue == NULL) {
        return pdFAIL;
    }

    return xQueueSend(
        ui_queue,
        &evt,
        0
    );
}


BaseType_t ui_send_event_from_isr(
    ui_event_t evt,
    BaseType_t *hp_task_woken
)
{
    if (ui_queue == NULL) {
        return pdFAIL;
    }

    return xQueueSendFromISR(
        ui_queue,
        &evt,
        hp_task_woken
    );
}


// ============================================================================
// UI Task
// ============================================================================

void ui_task(void *arg)
{
    gfx_canvas_t canvas;


    // ------------------------------------------------------------------------
    // Инициализация canvas
    // ------------------------------------------------------------------------

    if (gfx_canvas_init(
            &canvas,
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT
        ) != ESP_OK) {

        ESP_LOGE(TAG, "Failed to allocate canvas");

        vTaskDelete(NULL);

        return;
    }


    // ------------------------------------------------------------------------
    // Splash Screen
    // ------------------------------------------------------------------------

    current_screen = UI_SCREEN_SPLASH;

    ui_render(&canvas);

    vTaskDelay(pdMS_TO_TICKS(2000));


    // ------------------------------------------------------------------------
    // Main Menu
    // ------------------------------------------------------------------------

    current_screen = UI_SCREEN_MAIN_MENU;

    ui_render(&canvas);


    // ------------------------------------------------------------------------
    // Main Event Loop
    // ------------------------------------------------------------------------

    ui_event_t evt;

    while (1) {

    BaseType_t event_received;

    event_received = xQueueReceive(
        ui_queue,
        &evt,
        pdMS_TO_TICKS(1000)
    );


    // Если пришло событие кнопки
    if (event_received == pdTRUE) {

        ui_handle_event(
            evt,
            &canvas
        );
    }


    // Главное меню обновляем раз в секунду.
    // Это нужно для часов.
    if (current_screen == UI_SCREEN_MAIN_MENU) {

        ui_render(&canvas);
    }
}


    // Теоретически сюда выполнение никогда не дойдёт.

    gfx_canvas_deinit(&canvas);

    vTaskDelete(NULL);
}
