#include "include/display.h"
#include "spi_bus.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "display";

// ============================================================================
// Пины управления ST7789
// ============================================================================

#define DISP_RST_GPIO    16        // Reset
#define DISP_DC_GPIO     15        // Data/Command
#define DISP_CS_GPIO     7         // Chip Select
#define DISP_BL_GPIO     6         // Backlight (PWM или GPIO ON)

#define SPI_FREQ_HZ      (20 * 1000 * 1000)  // 20 MHz

// ============================================================================
// Состояние
// ============================================================================

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;

// ============================================================================
// Подсветка
// ============================================================================

static esp_err_t backlight_init(void)
{
    gpio_config_t bl_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISP_BL_GPIO,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    
    esp_err_t err = gpio_config(&bl_gpio_config);
    if (err != ESP_OK) {
        return err;
    }
    
    gpio_set_level(DISP_BL_GPIO, 1);
    ESP_LOGI(TAG, "Backlight ON");
    return ESP_OK;
}

// ============================================================================
// Инициализация дисплея
// ============================================================================

esp_err_t display_init(void)
{
    esp_err_t ret = ESP_OK;

    // 1. Инициализируем общую шину SPI (если еще не была поднята)
    ret = spi_bus_shared_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize shared SPI bus");

    // 2. Создаем интерфейс LCD IO поверх имеющейся шины SPI
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISP_CS_GPIO,
        .dc_gpio_num = DISP_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = SPI_FREQ_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
    };
    
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SHARED_SPI_HOST, &io_config, &s_io_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to create panel IO");
    ESP_LOGI(TAG, "LCD panel IO created (DC=%d, CS=%d)", DISP_DC_GPIO, DISP_CS_GPIO);

   // 3. Создаем драйвер панели ST7789
esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = DISP_RST_GPIO,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   // ← было RGB
    .bits_per_pixel = 16,
};
    
    ret = esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to create ST7789 panel");
    ESP_LOGI(TAG, "ST7789 panel driver created");

    // 4. Инициализируем ST7789 (с блокировкой шины)
    spi_bus_lock();
    
    ret = esp_lcd_panel_reset(s_panel_handle);
    if (ret != ESP_OK) {
        spi_bus_unlock();
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    
    ret = esp_lcd_panel_init(s_panel_handle);
    if (ret != ESP_OK) {
        spi_bus_unlock();
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // --- ИСПРАВЛЕНИЯ ОРИЕНТАЦИИ, ЗУМА И ЦВЕТОВ ---
    // Включаем swap_xy и зеркалирование X, чтобы дедсек встал ровно в правый нижний угол
  // --- Ориентация и цвета ---
esp_lcd_panel_swap_xy(s_panel_handle, true);
esp_lcd_panel_mirror(s_panel_handle, true, false);
esp_lcd_panel_invert_color(s_panel_handle, false);

// Критично для 170×320
esp_lcd_panel_set_gap(s_panel_handle, 0, 35);     // ← добавил

esp_lcd_panel_disp_on_off(s_panel_handle, true);
    
    spi_bus_unlock();
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 5. Включаем подсветку
    ret = backlight_init();
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize backlight");
    
    ESP_LOGI(TAG, "Display initialized successfully (320x240, RGB565)");
    return ESP_OK;
}

// ============================================================================
// Рисование на дисплей
// ============================================================================

esp_err_t display_draw_bitmap(int x0, int y0, int x1, int y1, const uint16_t *color_data)
{
    if (s_panel_handle == NULL) {
        ESP_LOGE(TAG, "display_draw_bitmap: Panel not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Захватываем мьютекс, чтобы SD-карта не влезала в транзакцию кадров
    spi_bus_lock();
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle, x0, y0, x1, y1, color_data);
    spi_bus_unlock();
    
    return err;
}

// ============================================================================
// Заливка цветом
// ============================================================================

esp_err_t display_fill_color(uint16_t color)
{
    size_t pixel_count = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;
    
    uint16_t *framebuffer = heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer (%zu bytes)", pixel_count * sizeof(uint16_t));
        return ESP_ERR_NO_MEM;
    }
    
    uint32_t color32 = (uint32_t)color | ((uint32_t)color << 16);
    for (size_t i = 0; i < pixel_count / 2; i++) {
        ((uint32_t *)framebuffer)[i] = color32;
    }
    if (pixel_count % 2) {
        framebuffer[pixel_count - 1] = color;
    }
    
    esp_err_t err = display_draw_bitmap(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, framebuffer);
    free(framebuffer);
    
    return err;
}

// ============================================================================
// Получить handle панели
// ============================================================================

esp_lcd_panel_handle_t display_get_panel_handle(void)
{
    return s_panel_handle;
}
