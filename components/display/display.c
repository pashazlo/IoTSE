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
// Пины
// ============================================================================

#define DISP_RST_GPIO   16
#define DISP_DC_GPIO    15
#define DISP_CS_GPIO    7
#define DISP_BL_GPIO    6

// ============================================================================
// Состояние драйвера
// ============================================================================

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static SemaphoreHandle_t s_flush_done_sem = NULL;

// ============================================================================
// ISR Callback
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
// Backlight
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
        ESP_LOGI(TAG, "Backlight ON");
    }
    return err;
}

// ============================================================================
// Управление ориентацией через MADCTL
// ============================================================================

esp_err_t display_set_rotation(uint8_t rotation)
{
    if (s_io_handle == NULL) {
        ESP_LOGE(TAG, "display_set_rotation: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    uint8_t madctl = 0x00;
    
    switch (rotation % 4) {
        case 0:  // 0°
            madctl = 0x00;
            break;
        case 1:  // 90° — альбом
            madctl = 0x60;
            break;
        case 2:  // 180°
            madctl = 0xC0;
            break;
        case 3:  // 270°
            madctl = 0xA0;
            break;
    }
    
    // Включаем BGR (если нужно — поменяй на 0x00 для RGB)
    madctl |= 0x08;  // BGR
    
    ESP_LOGI(TAG, "MADCTL=0x%02X", madctl);
    return esp_lcd_panel_io_tx_param(s_io_handle, 0x36, &madctl, 1);
}

// ============================================================================
// Инициализация
// ============================================================================

esp_err_t display_init(void)
{
    esp_err_t ret = ESP_OK;

    // Семафор
    s_flush_done_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done_sem != NULL, ESP_ERR_NO_MEM, TAG, "sem alloc failed");

    // ========================================================================
    // SPI интерфейс
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
    s_io_handle = io_handle;
    ESP_LOGI(TAG, "SPI interface ready");

    // ========================================================================
    // ST7789 контроллер
    // ========================================================================

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISP_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "panel st7789 init failed");

    // ========================================================================
    // Инициализация по даташиту
    // ========================================================================

    // Reset
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    // Sleep Out
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "init failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    // COLMOD — 16-bit RGB565
    uint8_t colmod = 0x55;
    esp_lcd_panel_io_tx_param(s_io_handle, 0x3A, &colmod, 1);
    ESP_LOGI(TAG, "COLMOD=0x%02X", colmod);

    // ========================================================================
    // ТЕСТОВЫЙ РЕЖИМ — посмотри какая комбинация работает
    // ========================================================================

    ESP_LOGI(TAG, "=== TEST MODE START ===");
    ESP_LOGI(TAG, "Watch the screen and note which test looks correct");
    
    // Тест 1: Invert ON, BGR ON
    ESP_LOGI(TAG, "Test 1: Invert=ON, BGR=ON");
    esp_lcd_panel_invert_color(s_panel_handle, true);
    display_set_rotation(1);  // BGR включен (0x68)
    display_fill_color(0xFFFF);  // Белый
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0xF800);  // Красный
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0x07E0);  // Зеленый
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0x001F);  // Синий
    vTaskDelay(pdMS_TO_TICKS(1500));
    
    // Тест 2: Invert ON, BGR OFF
    ESP_LOGI(TAG, "Test 2: Invert=ON, BGR=OFF");
    uint8_t madctl_no_bgr = 0x60;
    esp_lcd_panel_io_tx_param(s_io_handle, 0x36, &madctl_no_bgr, 1);
    display_fill_color(0xFFFF);
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0xF800);
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0x07E0);
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0x001F);
    vTaskDelay(pdMS_TO_TICKS(1500));
    
    // Тест 3: Invert OFF, BGR ON
    ESP_LOGI(TAG, "Test 3: Invert=OFF, BGR=ON");
    esp_lcd_panel_invert_color(s_panel_handle, false);
    display_set_rotation(1);  // BGR включен
    display_fill_color(0xFFFF);
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0xF800);
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0x07E0);
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0x001F);
    vTaskDelay(pdMS_TO_TICKS(1500));
    
    // Тест 4: Invert OFF, BGR OFF
    ESP_LOGI(TAG, "Test 4: Invert=OFF, BGR=OFF");
    madctl_no_bgr = 0x60;
    esp_lcd_panel_io_tx_param(s_io_handle, 0x36, &madctl_no_bgr, 1);
    display_fill_color(0xFFFF);
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0xF800);
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0x07E0);
    vTaskDelay(pdMS_TO_TICKS(1500));
    display_fill_color(0x001F);
    vTaskDelay(pdMS_TO_TICKS(1500));
    
    ESP_LOGI(TAG, "=== TEST COMPLETE ===");
    ESP_LOGI(TAG, "Which test had correct colors?");
    ESP_LOGI(TAG, "  - White should be WHITE (not black)");
    ESP_LOGI(TAG, "  - Red should be RED");
    ESP_LOGI(TAG, "  - Green should be GREEN");
    ESP_LOGI(TAG, "  - Blue should be BLUE");
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Based on the test, change the settings below:");
    ESP_LOGI(TAG, "  - Invert ON/OFF (line ~185)");
    ESP_LOGI(TAG, "  - BGR ON/OFF (line ~90)");
    
    // ========================================================================
    // ВРЕМЕННО: оставляем Test 1 (Invert ON, BGR ON) как рабочий вариант
    // Если нужен другой — поменяй ниже
    // ========================================================================
    
    ESP_LOGI(TAG, "Using Test 1 as default (Invert ON, BGR ON)");
    esp_lcd_panel_invert_color(s_panel_handle, true);
    display_set_rotation(1);
    display_fill_color(0xFFFF);
    
    ESP_LOGI(TAG, "Display ready!");
    return ESP_OK;
}

// ============================================================================
// Отрисовка
// ============================================================================

esp_err_t display_flush(int x1, int y1, int x2, int y2, const uint16_t *color_data)
{
    if (s_panel_handle == NULL) {
        ESP_LOGE(TAG, "display_flush: not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    spi_bus_lock();

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle, x1, y1, x2, y2, color_data);
    if (err == ESP_OK) {
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
// Заливка
// ============================================================================

esp_err_t display_fill_color(uint16_t color)
{
    size_t pixel_count = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;

    uint16_t *framebuffer = heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "display_fill_color: malloc failed");
        return ESP_ERR_NO_MEM;
    }

    uint32_t color32 = (uint32_t)color | ((uint32_t)color << 16);
    for (size_t i = 0; i < pixel_count / 2; i++) {
        ((uint32_t*)framebuffer)[i] = color32;
    }
    if (pixel_count % 2) {
        framebuffer[pixel_count - 1] = color;
    }

    esp_err_t err = display_flush(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, framebuffer);
    free(framebuffer);
    return err;
}
