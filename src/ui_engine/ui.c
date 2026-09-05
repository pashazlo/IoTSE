```c
#include "ui.h"

#include "display.h"
#include "gfx_canvas.h"

#include "ui_screen.h"
#include "ui_render.h"
#include "ui_controller.h"

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
    (void)arg;

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

    ui_screen_set(UI_SCREEN_SPLASH);

    ui_render(&canvas);

    vTaskDelay(pdMS_TO_TICKS(2000));


    // ------------------------------------------------------------------------
    // Main Menu
    // ------------------------------------------------------------------------

    ui_screen_set(UI_SCREEN_MAIN_MENU);

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

            ui_controller_handle_event(
                evt,
                &canvas
            );
        }


        // Главное меню обновляем раз в секунду.
        // Это нужно для часов.
        if (ui_screen_get() == UI_SCREEN_MAIN_MENU) {

            ui_render(&canvas);
        }
    }


    // Теоретически сюда выполнение никогда не дойдёт.

    gfx_canvas_deinit(&canvas);

    vTaskDelete(NULL);
}

