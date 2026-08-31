#include "spi_bus.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#iclude "ui.h"

static const char *TAG = "app_main";

/**
 * Application entry point.
 *
 * Initialization order:
 * 1. Initialize the shared SPI bus (creates mutex, configures GPIO)
 * 2. Initialize the ST7789 display driver
 * 3. Fill screen with a red test pattern
 * 4. Suspend main task (application can extend with real tasks here)
 */
void app_main(void)
{
    ESP_LOGI(TAG, "=== IoTSE Application Starting ===");

    // ========================================================================
    // Initialize SPI Bus (must be first, before any device drivers)
    // ========================================================================
    ESP_LOGI(TAG, "Initializing shared SPI bus...");
    esp_err_t err = spi_bus_shared_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "✓ SPI bus initialized");

    // ========================================================================
    // Initialize Display
    // ========================================================================
    ESP_LOGI(TAG, "Initializing ST7789 display...");
    err = display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "✓ Display initialized");

    // ========================================================================
    // Display Self-Test: Red Screen
    // ========================================================================
    ESP_LOGI(TAG, "Running display self-test (red screen)...");
    err = display_fill_color(0xF800);  // 0xF800 = bright red in RGB565
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✓ Display self-test passed");
    } else {
        ESP_LOGE(TAG, "Display self-test failed: %s", esp_err_to_name(err));
    }

    // ========================================================================
    // Application Ready
    // ========================================================================
    ESP_LOGI(TAG, "=== IoTSE Application Ready ===");
    ESP_LOGI(TAG, "Ready for additional tasks (WiFi, BLE, SD card, etc.)");

    // Suspend this task. Application logic can be added here or in other tasks:
    // - Create WiFi/BLE tasks
    // - Create SD card read/write tasks (they will use spi_bus_lock/unlock)
    // - Create UI update tasks (they will use display_flush)
    //
    // Example for later:
    //   xTaskCreate(wifi_task, "wifi", 4096, NULL, 5, NULL);
    //   xTaskCreate(sd_card_task, "sd_card", 4096, NULL, 4, NULL);
    //   xTaskCreate(ui_update_task, "ui", 4096, NULL, 3, NULL);

    vTaskSuspend(NULL);  // Suspend main task indefinitely
}
