#include "display.h"
#include "spi_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_types.h"

static const char *TAG = "display";

// Display-specific pins (SCK/MOSI live in spi_bus.h — shared with SD card)
#define DISP_RST_GPIO   16
#define DISP_DC_GPIO    15
#define DISP_CS_GPIO    7
#define DISP_BL_GPIO    6

// If colors look inverted on your panel, flip this to false and reflash.
#define DISP_INVERT_COLOR   true

// Adjust if your panel has a memory offset (common on 240x240 modules
// mounted on a 240x320 controller) or is mounted rotated/mirrored.
#define DISP_GAP_X   0
#define DISP_GAP_Y   0
#define DISP_SWAP_XY   false
#define DISP_MIRROR_X  false
#define DISP_MIRROR_Y  false

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static SemaphoreHandle_t s_flush_done_sem = NULL;

// Called from ISR context by esp_lcd once the DMA transfer actually finishes
static bool IRAM_ATTR notify_flush_done(esp_lcd_panel_io_handle_t io,
                                         esp_lcd_panel_io_event_data_t *edata,
                                         void *user_ctx)
{
    BaseType_t hp_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done_sem, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

static esp_err_t backlight_init(void)
{
    gpio_config_t bl_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISP_BL_GPIO,
    };
    esp_err_t err = gpio_config(&bl_gpio_config);
    if (err == ESP_OK) {
        gpio_set_level(DISP_BL_GPIO, 1);
    }
    return err;
}

esp_err_t display_init(void)
{
    esp_err_t ret = ESP_OK;

    s_flush_done_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done_sem != NULL, ESP_ERR_NO_MEM, TAG, "sem alloc failed");

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

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISP_RST_GPIO,
        .color_space = ESP_LCD_COLOR_SPACE_BGR,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "panel st7789 init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel_handle, DISP_INVERT_COLOR), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, DISP_SWAP_XY), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle, DISP_MIRROR_X, DISP_MIRROR_Y), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel_handle, DISP_GAP_X, DISP_GAP_Y), TAG, "set_gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "disp_on failed");

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");

    return ESP_OK;
}

esp_err_t display_flush(int x1, int y1, int x2, int y2, const uint16_t *color_data)
{
    if (s_panel_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Exclusive access to the physical bus for the whole flush,
    // including the wait for the DMA transfer to actually finish.
    spi_bus_lock();

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle, x1, y1, x2, y2, color_data);
    if (err == ESP_OK) {
        xSemaphoreTake(s_flush_done_sem, portMAX_DELAY);
    }

    spi_bus_unlock();
    return err;
}

esp_err_t display_fill_color(uint16_t color)
{
    size_t pixel_count = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;
    uint16_t *framebuffer = heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (framebuffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < pixel_count; i++) {
        framebuffer[i] = color;
    }

    esp_err_t err = display_flush(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, framebuffer);

    // Safe to free here: display_flush() only returns after the DMA
    // transfer has completed (see notify_flush_done / s_flush_done_sem).
    free(framebuffer);
    return err;
}
