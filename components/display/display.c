#include "include/display.h"
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
// Пины (ST7789)
// ============================================================================

#define DISP_RST_GPIO    16        // Reset
#define DISP_DC_GPIO     15        // Data/Command
#define DISP_CS_GPIO     7         // Chip Select
#define DISP_BL_GPIO     6         // Backlight (PWM или просто ON)
#define DISP_SCLK_GPIO   18        // SCK (Общая шина SPI2)
#define DISP_MOSI_GPIO   17        // MOSI (Общая шина SPI2)
#define DISP_MISO_GPIO   -1        // MISO не нужен для дисплея

#define SPI_FREQ_HZ      (20 * 1000 * 1000)  // 20 MHz

// ============================================================================
// Состояние
// ============================================================================

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;

// ============================================================================
// Backlight
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

    // ====================================================================
    // 1. Умная инициализация SPI (Безопасно для SD-карты)
    // ====================================================================
    
    spi_bus_config_t buscfg = {
        .sclk_io_num = DISP_SCLK_GPIO,
        .mosi_io_num = DISP_MOSI_GPIO,
        .miso_io_num = DISP_MISO_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
    };
    
    // Пытаемся инициализировать шину. 
    // Если SD-карта уже подняла SPI2_HOST, функция вернет ESP_ERR_INVALID_STATE, 
    // мы мягко игнорируем это и используем уже готовую шину!
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPI2 bus initialized by Display (SCK=%d, MOSI=%d)", DISP_SCLK_GPIO, DISP_MOSI_GPIO);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPI2 bus already initialized by SD card. Reusing bus.");
    } else {
        ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize SPI bus");
    }

    // ====================================================================
    // 2. Конфигурация LCD panel IO (SPI interface)
    // ====================================================================
    
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISP_CS_GPIO,
        .dc_gpio_num = DISP_DC_GPIO,
        .spi_mode = 0,                  // Mode 0: CPOL=0, CPHA=0
        .pclk_hz = SPI_FREQ_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
    };
    
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to create panel IO");
    ESP_LOGI(TAG, "LCD panel IO created (DC=%d, CS=%d)", DISP_DC_GPIO, DISP_CS_GPIO);

    // ====================================================================
    // 3. Конфигурация ST7789 panel driver
    // ====================================================================
    
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISP_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    
    ret = esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to create ST7789 panel");
    ESP_LOGI(TAG, "ST7789 panel driver created");

    // ====================================================================
    // 4. Инициализация ST7789 по даташиту
    // ====================================================================
    
    ret = esp_lcd_panel_reset(s_panel_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to reset panel");
    vTaskDelay(pdMS_TO_TICKS(10));
    
    ret = esp_lcd_panel_init(s_panel_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize panel");
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ret = esp_lcd_panel_swap_xy(s_panel_handle, true);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to swap XY");
    
    ret = esp_lcd_panel_mirror(s_panel_handle, false, true);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to mirror");
    
    ret = esp_lcd_panel_invert_color(s_panel_handle, false);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to set color inversion");
    
    ret = esp_lcd_panel_disp_on_off(s_panel_handle, true);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to turn on display");
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // ====================================================================
    // 5. Инициализация подсветки
    // ====================================================================
    
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
    
    return esp_lcd_panel_draw_bitmap(s_panel_handle, x0, y0, x1, y1, color_data);
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
