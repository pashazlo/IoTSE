#include "spi_bus.h"
#include "display.h"
#include "storage.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "ui.h"
#include "buttons.h"


static const char *TAG = "app_main";


// ============================================================================
// Мост: Buttons -> UI
// ============================================================================

static void input_bridge_task(void *arg)
{
    QueueHandle_t button_queue = buttons_get_queue();

    if (button_queue == NULL) {
        ESP_LOGE(TAG, "Button queue is NULL");
        vTaskDelete(NULL);
        return;
    }

    button_event_t button_evt;

    while (1) {

        if (xQueueReceive(
                button_queue,
                &button_evt,
                portMAX_DELAY
            ) == pdTRUE) {

            // Нас интересует только момент нажатия.
            // События отпускания пока игнорируем.
            if (button_evt.kind != BTN_EVENT_PRESSED) {
                continue;
            }

            switch (button_evt.button) {

                case BTN_UP:
                    ui_send_event(UI_EVT_UP);
                    break;

                case BTN_DOWN:
                    ui_send_event(UI_EVT_DOWN);
                    break;

                case BTN_LEFT:
                    ui_send_event(UI_EVT_LEFT);
                    break;

                case BTN_RIGHT:
                    ui_send_event(UI_EVT_RIGHT);
                    break;

                case BTN_SELECT:
                    ui_send_event(UI_EVT_SELECT);
                    break;

                default:
                    break;
            }
        }
    }
}


// ============================================================================
// Точка входа
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "=== Запуск приложения IoTSE ===");


    // ========================================================================
    // 0. Storage (NVS + FAT) — ПЕРВЫМ ДЕЛОМ, до всего остального.
    //    NVS в ESP-IDF по соглашению должен быть готов раньше любых
    //    других компонентов (в первую очередь — будущего Wi-Fi-стека).
    // ========================================================================

    ESP_LOGI(TAG, "Инициализация NVS...");

    esp_err_t err = storage_nvs_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка инициализации NVS: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "✓ NVS инициализирован");

    ESP_LOGI(TAG, "Монтирование FAT-раздела...");

    err = storage_fat_init();

    if (err != ESP_OK) {
        // Не считаем это фатальным для остальной прошивки (дисплей
        // и меню могут работать и без файловой системы) — просто
        // предупреждаем и идём дальше. Если для вашей логики
        // отсутствие storage критично — замените на return.
        ESP_LOGW(TAG, "FAT-раздел не смонтирован: %s (продолжаем без него)",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "✓ FAT-раздел смонтирован на %s", STORAGE_FAT_MOUNT_POINT);
    }


    // ========================================================================
    // 1. SPI
    // ========================================================================

    ESP_LOGI(TAG, "Инициализация общей шины SPI...");

    err = spi_bus_shared_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка инициализации SPI: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "✓ Шина SPI инициализирована");


    // ========================================================================
    // 2. Buttons
    // ========================================================================

    ESP_LOGI(TAG, "Инициализация кнопок...");

    err = buttons_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка инициализации кнопок: %s",
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "✓ Кнопки инициализированы");


    // ========================================================================
    // 3. Display
    // ========================================================================

    ESP_LOGI(TAG, "Инициализация дисплея ST7789...");

    err = display_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка инициализации дисплея: %s",
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "✓ Дисплей инициализирован");


    // ========================================================================
    // 4. UI Queue
    // ========================================================================

    ESP_LOGI(TAG, "Инициализация UI...");

    err = ui_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ошибка инициализации UI: %s",
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "✓ UI инициализирован");


    // ========================================================================
    // 5. UI Task
    // ========================================================================

    BaseType_t task_created = xTaskCreate(
        ui_task,
        "ui",
        4096,
        NULL,
        4,
        NULL
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Не удалось создать задачу ui_task");
        return;
    }

    ESP_LOGI(TAG, "✓ Задача интерфейса создана");


    // ========================================================================
    // 6. Input Bridge
    // ========================================================================

    task_created = xTaskCreate(
        input_bridge_task,
        "input_bridge",
        2048,
        NULL,
        5,
        NULL
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Не удалось создать input_bridge_task");
        return;
    }

    ESP_LOGI(TAG, "✓ Input bridge запущен");


    // ========================================================================
    // Готово
    // ========================================================================

    ESP_LOGI(TAG, "=== Приложение IoTSE готово ===");

    vTaskSuspend(NULL);
}
