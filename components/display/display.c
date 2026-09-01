#include "display.h"
#include "spi_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_types.h"

static const char *TAG = "display";

// ============================================================================
// Display Hardware Configuration
// ============================================================================

// Display-specific pins (SCK/MOSI are shared with SD card, defined in spi_bus.h)
#define DISP_RST_GPIO   16    // Reset pin (active low)
#define DISP_DC_GPIO    15    // Data/Command select pin
#define DISP_CS_GPIO    7     // Chip Select (active low)
#define DISP_BL_GPIO    6     // Backlight enable (0=off, 1=on)

// ============================================================================
// Display State
// ============================================================================

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;  // <-- ДОБАВЛЕНО: сохраняем для команд

// Semaphore signaled by ISR when DMA transfer to display completes.
static SemaphoreHandle_t s_flush_done_sem = NULL;

// ============================================================================
// ISR Callbacks
// ============================================================================

static bool IRAM_ATTR notify_flush_done(esp_lcd_panel_io_handle_t io,
                                        esp_lcd_panel_io_event_data_t *edata,
                                        void *user_ctx)
{
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done_sem, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

// ============================================================================
// Initialization Helpers
// ============================================================================

static esp_err_t backlight_init(void)
{
    gpio_config_t bl_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISP_BL_GPIO,
    };
    esp_err_t err = gpio_config(&bl_gpio_config);
    if (err == ESP_OK) {
        gpio_set_level(DISP_BL_GPIO, 1);
        ESP_LOGI(TAG, "Backlight initialized (GPIO %d)", DISP_BL_GPIO);
    } else {
        ESP_LOGE(TAG, "Backlight GPIO config failed: %s", esp_err_to_name(err));
    }
    return err;
}

// ============================================================================
// НОВАЯ ФУНКЦИЯ: Установка ориентации по даташиту
// ============================================================================

esp_err_t display_set_rotation(uint8_t rotation)
{
    if (s_io_handle == NULL) {
        ESP_LOGE(TAG, "display_set_rotation: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // MADCTL (36h) из даташита, секция 9.1.28
    // БИТЫ:
    // D7: MY - Mirror Y
    // D6: MX - Mirror X  
    // D5: MV - Swap XY (включает альбомный режим)
    // D4: ML - Vertical refresh order
    // D3: RGB - 0=RGB, 1=BGR
    // D2: MH - Horizontal refresh order
    
    uint8_t madctl = 0x00;
    
    // ВАЖНО: для альбомного режима нужен MV=1
    // Остальные биты подбираем под нужный поворот
    switch (rotation % 4) {
        case 0:  // 0° — портрет
            madctl = 0x00;
            break;
        case 1:  // 90° — альбом (поворот вправо)
            madctl = 0x60;  // MV=1, MX=1
            break;
        case 2:  // 180° — портрет вверх ногами
            madctl = 0xC0;  // MY=1, MX=1
            break;
        case 3:  // 270° — альбом (поворот влево)
            madctl = 0xA0;  // MV=1, MY=1
            break;
    }
    
    // ПРАВИЛЬНАЯ настройка BGR/RGB:
    // Твой дисплей работает с BGR (проверено экспериментально)
    // Бит D3=1 включает BGR
    madctl |= 0x08;  // <-- ВКЛЮЧАЕМ BGR, т.к. у тебя были проблемы с цветами
    
    ESP_LOGI(TAG, "Setting MADCTL = 0x%02X (rotation %d)", madctl, rotation);
    return esp_lcd_panel_io_tx_param(s_io_handle, 0x36, &madctl, 1);
}

// ============================================================================
// Public Functions
// ============================================================================

esp_err_t display_init(void)
{
    esp_err_t ret = ESP_OK;

    // Create semaphore
    s_flush_done_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done_sem != NULL, ESP_ERR_NO_MEM, TAG, "sem alloc failed");
    ESP_LOGI(TAG, "Flush-done semaphore created");

    // ========================================================================
    // Configure SPI interface
    // ========================================================================

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISP_CS_GPIO,
        .dc_gpio_num = DISP_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = notify_flush_done,
        .user_ctx = NULL,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SHARED_SPI_HOST, &io_config, &io_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "panel io init failed");
    ESP_LOGI(TAG, "SPI I/O interface created (CS=%d, DC=%d, 40MHz)", DISP_CS_GPIO, DISP_DC_GPIO);
    
    // СОХРАНЯЕМ io_handle для отправки команд
    s_io_handle = io_handle;

    // ========================================================================
    // Configure ST7789 display controller
    // ========================================================================

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISP_RST_GPIO,
        // ИЗМЕНЕНО: пробуем RGB, но MADCTL потом переключит в BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "panel st7789 init failed");
    ESP_LOGI(TAG, "ST7789 controller created (RST=%d, 16-bit)", DISP_RST_GPIO);

    // ========================================================================
    // Initialize display по даташиту (секция 8.16)
    // ========================================================================

    // 1. Hardware reset
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "reset failed");
    ESP_LOGI(TAG, "Display hardware reset complete");
    vTaskDelay(pdMS_TO_TICKS(10));  // Небольшая пауза после reset

    // 2. Init panel (выход из сна)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "init failed");
    ESP_LOGI(TAG, "Display initialized and powered on");
    vTaskDelay(pdMS_TO_TICKS(120));  // ВАЖНО: 120ms после SLPOUT (даташит секция 7.4.5)

    // 3. Явно устанавливаем COLMOD (формат пикселя) - 16-bit RGB565
    //    Даташит секция 9.1.32, стр. 224
    uint8_t colmod = 0x55;  // 0x55 = 16-bit/pixel (RGB 5-6-5)
    ESP_LOGI(TAG, "Setting COLMOD = 0x%02X (16-bit RGB565)", colmod);
    esp_lcd_panel_io_tx_param(s_io_handle, 0x3A, &colmod, 1);

    // 4. Устанавливаем альбомную ориентацию (90°)
    //    Вызов нашей новой функции
    display_set_rotation(1);  // 1 = 90° (альбом)

    // 5. ИЗМЕНЕНО: выключаем инверсию — она только портит цвета
    //    Если нужна инверсия — раскомментируй, но с BGR она не нужна
    // ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel_handle, false), TAG, "invert failed");
    
    // 6. Отключаем swap/mirror, т.к. управляем через MADCTL
    //    Это предотвращает конфликты с нашей настройкой ориентации
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, false), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle, false, false), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel_handle, 0, 0), TAG, "set_gap failed");

    // 7. Включаем дисплей
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "disp_on failed");
    ESP_LOGI(TAG, "Display turned on (albom mode, 320x240)");

    // 8. Backlight
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");

    // 9. Тестовая заливка (чтобы сразу увидеть результат)
    ESP_LOGI(TAG, "Testing display with RED color...");
    display_fill_color(0xF800);  // Красный
    vTaskDelay(pdMS_TO_TICKS(500));
    display_fill_color(0x07E0);  // Зеленый
    vTaskDelay(pdMS_TO_TICKS(500));
    display_fill_color(0x001F);  // Синий
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Возвращаем белый фон для UI
    display_fill_color(0xFFFF);  // Белый

    ESP_LOGI(TAG, "Display fully initialized and ready!");
    return ESP_OK;
}

// ============================================================================
// display_flush — БЕЗ ИЗМЕНЕНИЙ (сохраняем твой код)
// ============================================================================

esp_err_t display_flush(int x1, int y1, int x2, int y2, const uint16_t *color_data)
{
    if (s_panel_handle == NULL) {
        ESP_LOGE(TAG, "display_flush: Display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    spi_bus_lock();

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle, x1, y1, x2, y2, color_data);
    if (err == ESP_OK) {
        // Ждем завершения DMA
        if (xSemaphoreTake(s_flush_done_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "display_flush: DMA timeout!");
            spi_bus_unlock();
            return ESP_ERR_TIMEOUT;
        }
    } else {
        ESP_LOGE(TAG, "display_flush: draw_bitmap failed: %s", esp_err_to_name(err));
    }

    spi_bus_unlock();
    return err;
}

// ============================================================================
// display_fill_color — БЕЗ ИЗМЕНЕНИЙ (сохраняем твой код)
// ============================================================================

esp_err_t display_fill_color(uint16_t color)
{
    size_t pixel_count = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;

    uint16_t *framebuffer = heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "display_fill_color: Failed to allocate %zu bytes", pixel_count * sizeof(uint16_t));
        return ESP_ERR_NO_MEM;
    }

    // Оптимизация: заполняем через 32-битные блоки
    uint32_t color32 = (uint32_t)color | ((uint32_t)color << 16);
    for (size_t i = 0; i < pixel_count / 2; i++) {
        ((uint32_t*)framebuffer)[i] = color32;
    }
    // Если нечетное количество пикселей
    if (pixel_count % 2) {
        framebuffer[pixel_count - 1] = color;
    }

    esp_err_t err = display_flush(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, framebuffer);
    free(framebuffer);

    if (err == ESP_OK) {
        ESP_LOGV(TAG, "display_fill_color: Screen filled with color 0x%04X", color);
    }
    return err;
}
