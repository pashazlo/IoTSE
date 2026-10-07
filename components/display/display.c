#include "include/display.h"

#include <stdbool.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "spi_bus.h"

static const char *TAG = "display";

#define DISP_RST_GPIO 16
#define DISP_DC_GPIO  15
#define DISP_CS_GPIO  SPI_BUS_DISPLAY_CS_GPIO
#define DISP_BL_GPIO  6

#define SPI_FREQ_HZ        (40 * 1000 * 1000)
#define DMA_STRIP_ROWS     10
#define DMA_BUFFER_COUNT   2
#define DMA_WAIT_MS        1000
#define SPI_LOCK_WAIT_MS   1000

static esp_lcd_panel_handle_t s_panel_handle;
static esp_lcd_panel_io_handle_t s_io_handle;
static uint16_t *s_dma_buffers[DMA_BUFFER_COUNT];
static SemaphoreHandle_t s_dma_done;
static bool s_transport_quarantined;

static bool color_transfer_done(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_ctx
)
{
    (void)panel_io;
    (void)event_data;
    (void)user_ctx;

    BaseType_t high_task_woken = pdFALSE;
    if (s_dma_done != NULL) {
        xSemaphoreGiveFromISR(s_dma_done, &high_task_woken);
    }
    return high_task_woken == pdTRUE;
}

static esp_err_t backlight_init(void)
{
    gpio_config_t config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISP_BL_GPIO,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(DISP_BL_GPIO, 1);
    ESP_LOGI(TAG, "Backlight ON");
    return ESP_OK;
}

static void free_dma_buffers(void)
{
    for (size_t i = 0; i < DMA_BUFFER_COUNT; ++i) {
        if (s_dma_buffers[i] != NULL) {
            heap_caps_free(s_dma_buffers[i]);
            s_dma_buffers[i] = NULL;
        }
    }
}

static esp_err_t allocate_dma_transport(void)
{
    s_dma_done = xSemaphoreCreateCounting(DMA_BUFFER_COUNT, 0);
    if (s_dma_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t bytes =
        (size_t)DISPLAY_WIDTH * DMA_STRIP_ROWS * sizeof(uint16_t);
    for (size_t i = 0; i < DMA_BUFFER_COUNT; ++i) {
        s_dma_buffers[i] = heap_caps_malloc(
            bytes,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
        );
        if (s_dma_buffers[i] == NULL) {
            free_dma_buffers();
            vSemaphoreDelete(s_dma_done);
            s_dma_done = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "DMA ping-pong allocated: %u x %zu bytes",
             DMA_BUFFER_COUNT, bytes);
    return ESP_OK;
}

esp_err_t display_init(void)
{
    ESP_RETURN_ON_ERROR(
        spi_bus_shared_init(),
        TAG,
        "Failed to initialize shared SPI bus"
    );
    ESP_RETURN_ON_ERROR(
        allocate_dma_transport(),
        TAG,
        "Failed to allocate DMA ping-pong"
    );

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISP_CS_GPIO,
        .dc_gpio_num = DISP_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = SPI_FREQ_HZ,
        .trans_queue_depth = DMA_BUFFER_COUNT,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = color_transfer_done,
        .user_ctx = NULL,
    };

    esp_err_t err = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SHARED_SPI_HOST,
        &io_config,
        &s_io_handle
    );
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to create panel IO");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISP_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(
        s_io_handle,
        &panel_config,
        &s_panel_handle
    );
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to create ST7789 panel");

    if (!spi_bus_lock(pdMS_TO_TICKS(SPI_LOCK_WAIT_MS))) {
        return ESP_ERR_TIMEOUT;
    }

    err = esp_lcd_panel_reset(s_panel_handle);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = esp_lcd_panel_init(s_panel_handle);
    }
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
        err = esp_lcd_panel_swap_xy(s_panel_handle, true);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_mirror(s_panel_handle, true, false);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_invert_color(s_panel_handle, true);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_set_gap(s_panel_handle, 0, 35);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(s_panel_handle, true);
    }

    spi_bus_unlock();
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "Failed to initialize backlight");

    ESP_LOGI(TAG,
             "Display initialized: 320x170 RGB565, SPI %u MHz, DMA ping-pong %u rows",
             (unsigned int)(SPI_FREQ_HZ / 1000000), DMA_STRIP_ROWS);
    return ESP_OK;
}

static esp_err_t wait_for_one_transfer(size_t *in_flight)
{
    if (*in_flight == 0) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_dma_done, pdMS_TO_TICKS(DMA_WAIT_MS)) != pdTRUE) {
        /* DMA ownership is unknown. Buffers stay allocated and the SPI mutex
           deliberately remains locked so another device cannot collide. */
        s_transport_quarantined = true;
        ESP_LOGE(TAG, "DMA completion timeout; display SPI quarantined");
        return ESP_ERR_TIMEOUT;
    }
    --(*in_flight);
    return ESP_OK;
}

static esp_err_t queue_strip(
    uint16_t *dma_buffer,
    int x0,
    int y0,
    int x1,
    int y1,
    size_t *in_flight
)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel_handle,
        x0,
        y0,
        x1,
        y1,
        dma_buffer
    );
    if (err == ESP_OK) {
        ++(*in_flight);
    }
    return err;
}

esp_err_t display_draw_bitmap_stride(
    int x0,
    int y0,
    int x1,
    int y1,
    const uint16_t *color_data,
    size_t source_stride_pixels
)
{
    if (s_panel_handle == NULL || s_dma_done == NULL ||
        s_dma_buffers[0] == NULL || s_dma_buffers[1] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_transport_quarantined) {
        return ESP_ERR_INVALID_STATE;
    }
    if (color_data == NULL || x0 < 0 || y0 < 0 ||
        x1 > DISPLAY_WIDTH || y1 > DISPLAY_HEIGHT ||
        x0 >= x1 || y0 >= y1) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t width = (size_t)(x1 - x0);
    if (source_stride_pixels < width) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!spi_bus_lock(pdMS_TO_TICKS(SPI_LOCK_WAIT_MS))) {
        return ESP_ERR_TIMEOUT;
    }

    /* No completion token may survive from a previous fully drained frame. */
    while (xSemaphoreTake(s_dma_done, 0) == pdTRUE) {
    }

    const size_t capacity_pixels =
        (size_t)DISPLAY_WIDTH * DMA_STRIP_ROWS;
    const size_t rows_per_strip = capacity_pixels / width;
    size_t in_flight = 0;
    size_t next_buffer = 0;
    esp_err_t err = ESP_OK;

    for (int strip_y = y0; strip_y < y1 && err == ESP_OK;) {
        if (in_flight == DMA_BUFFER_COUNT) {
            err = wait_for_one_transfer(&in_flight);
            if (err != ESP_OK) {
                break;
            }
        }

        size_t rows = (size_t)(y1 - strip_y);
        if (rows > rows_per_strip) {
            rows = rows_per_strip;
        }

        uint16_t *target = s_dma_buffers[next_buffer];
        const size_t source_row = (size_t)(strip_y - y0);
        for (size_t row = 0; row < rows; ++row) {
            const uint16_t *source = color_data +
                (source_row + row) * source_stride_pixels;
            uint16_t *target_row = target + row * width;
            for (size_t x = 0; x < width; ++x) {
                target_row[x] = __builtin_bswap16(source[x]);
            }
        }

        err = queue_strip(
            target,
            x0,
            strip_y,
            x1,
            strip_y + (int)rows,
            &in_flight
        );
        if (err == ESP_OK) {
            strip_y += (int)rows;
            next_buffer = (next_buffer + 1) % DMA_BUFFER_COUNT;
        }
    }

    while (in_flight > 0 && !s_transport_quarantined) {
        esp_err_t wait_err = wait_for_one_transfer(&in_flight);
        if (err == ESP_OK) {
            err = wait_err;
        }
    }

    if (!s_transport_quarantined) {
        spi_bus_unlock();
    }
    return err;
}

esp_err_t display_draw_bitmap(
    int x0,
    int y0,
    int x1,
    int y1,
    const uint16_t *color_data
)
{
    if (x1 <= x0) {
        return ESP_ERR_INVALID_ARG;
    }
    return display_draw_bitmap_stride(
        x0,
        y0,
        x1,
        y1,
        color_data,
        (size_t)(x1 - x0)
    );
}

esp_err_t display_update_rect(
    uint16_t x1,
    uint16_t y1,
    uint16_t x2,
    uint16_t y2,
    const uint16_t *pixel_data
)
{
    if (pixel_data == NULL || x1 >= x2 || y1 >= y2 ||
        x2 > DISPLAY_WIDTH || y2 > DISPLAY_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    return display_draw_bitmap(
        (int)x1,
        (int)y1,
        (int)x2,
        (int)y2,
        pixel_data
    );
}

esp_err_t display_fill_color(uint16_t color)
{
    if (s_panel_handle == NULL || s_dma_done == NULL ||
        s_dma_buffers[0] == NULL || s_dma_buffers[1] == NULL ||
        s_transport_quarantined) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!spi_bus_lock(pdMS_TO_TICKS(SPI_LOCK_WAIT_MS))) {
        return ESP_ERR_TIMEOUT;
    }

    while (xSemaphoreTake(s_dma_done, 0) == pdTRUE) {
    }

    const uint16_t swapped = __builtin_bswap16(color);
    const size_t pixels_per_buffer =
        (size_t)DISPLAY_WIDTH * DMA_STRIP_ROWS;
    for (size_t buffer = 0; buffer < DMA_BUFFER_COUNT; ++buffer) {
        for (size_t pixel = 0; pixel < pixels_per_buffer; ++pixel) {
            s_dma_buffers[buffer][pixel] = swapped;
        }
    }

    size_t in_flight = 0;
    size_t next_buffer = 0;
    esp_err_t err = ESP_OK;
    for (int y = 0; y < DISPLAY_HEIGHT && err == ESP_OK; y += DMA_STRIP_ROWS) {
        if (in_flight == DMA_BUFFER_COUNT) {
            err = wait_for_one_transfer(&in_flight);
            if (err != ESP_OK) {
                break;
            }
        }

        int y1 = y + DMA_STRIP_ROWS;
        if (y1 > DISPLAY_HEIGHT) {
            y1 = DISPLAY_HEIGHT;
        }
        err = queue_strip(
            s_dma_buffers[next_buffer],
            0,
            y,
            DISPLAY_WIDTH,
            y1,
            &in_flight
        );
        next_buffer = (next_buffer + 1) % DMA_BUFFER_COUNT;
    }

    while (in_flight > 0 && !s_transport_quarantined) {
        esp_err_t wait_err = wait_for_one_transfer(&in_flight);
        if (err == ESP_OK) {
            err = wait_err;
        }
    }

    if (!s_transport_quarantined) {
        spi_bus_unlock();
    }
    return err;
}

esp_lcd_panel_handle_t display_get_panel_handle(void)
{
    return s_panel_handle;
}
