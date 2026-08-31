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
#define DISP_CS_GPIO    7     // Chip Select (active low) — only Display listens when CS=low
#define DISP_BL_GPIO    6     // Backlight enable (0=off, 1=on)

// Color and orientation configuration
// If colors look inverted on your panel, flip this to false and reflash.
#define DISP_INVERT_COLOR   true

// Adjust if your panel has a memory offset (common on 240x240 modules
// mounted on a 240x320 controller) or is mounted rotated/mirrored.
#define DISP_GAP_X      0       // Horizontal offset (pixels)
#define DISP_GAP_Y      0       // Vertical offset (pixels)
#define DISP_SWAP_XY    true   // Swap X/Y (180 rotation)
#define DISP_MIRROR_X   false   // Mirror horizontally
#define DISP_MIRROR_Y   false   // Mirror vertically

// ============================================================================
// Display State
// ============================================================================

static esp_lcd_panel_handle_t s_panel_handle = NULL;

// Semaphore signaled by ISR when DMA transfer to display completes.
// Used to synchronize display_flush() with hardware completion.
static SemaphoreHandle_t s_flush_done_sem = NULL;

// ============================================================================
// ISR Callbacks
// ============================================================================

/**
 * Callback invoked by esp_lcd ISR when the DMA transfer to the display completes.
 *
 * This runs in ISR context — must be fast and use ISR-safe FreeRTOS calls.
 * We simply signal s_flush_done_sem so display_flush() can unblock.
 *
 * @param io          LCD I/O handle
 * @param edata       Event data from esp_lcd
 * @param user_ctx    User context (unused)
 * @return true if a higher-priority task was woken (for FreeRTOS scheduler)
 */
static bool IRAM_ATTR notify_flush_done(esp_lcd_panel_io_handle_t io,
                                        esp_lcd_panel_io_event_data_t *edata,
                                        void *user_ctx)
{
    BaseType_t hp_task_woken = pdFALSE;
    // Give the semaphore to wake display_flush() (if waiting)
    xSemaphoreGiveFromISR(s_flush_done_sem, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

// ============================================================================
// Initialization Helpers
// ============================================================================

/**
 * Initialize the display backlight GPIO and turn it on.
 *
 * The backlight (GPIO 6) is a simple on/off control:
 * - 0 = off (display dark)
 * - 1 = on (display bright)
 *
 * @return ESP_OK on success, error from gpio_config() on failure
 */
static esp_err_t backlight_init(void)
{
    gpio_config_t bl_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISP_BL_GPIO,
    };
    esp_err_t err = gpio_config(&bl_gpio_config);
    if (err == ESP_OK) {
        // Turn on the backlight
        gpio_set_level(DISP_BL_GPIO, 1);
        ESP_LOGI(TAG, "Backlight initialized (GPIO %d)", DISP_BL_GPIO);
    } else {
        ESP_LOGE(TAG, "Backlight GPIO config failed: %s", esp_err_to_name(err));
    }
    return err;
}

// ============================================================================
// Public Functions
// ============================================================================

/**
 * Initialize the ST7789 display driver and attach it to the shared SPI bus.
 *
 * Must be called AFTER spi_bus_shared_init() succeeds.
 *
 * Steps:
 * 1. Create a binary semaphore to sync with DMA completion ISR
 * 2. Configure SPI I/O interface (CS, DC, speed, DMA callback)
 * 3. Configure and initialize the ST7789 controller
 * 4. Set color space, orientation, and gap offsets
 * 5. Turn on the display and backlight
 *
 * @return ESP_OK on success, various ESP_ERR_* codes on failure
 */
esp_err_t display_init(void)
{
    esp_err_t ret = ESP_OK;

    // Create semaphore for synchronizing with DMA completion ISR
    s_flush_done_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done_sem != NULL, ESP_ERR_NO_MEM, TAG, "sem alloc failed");
    ESP_LOGI(TAG, "Flush-done semaphore created");

    // ========================================================================
    // Configure SPI interface (I/O layer between host and ST7789)
    // ========================================================================

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISP_CS_GPIO,              // Chip select pin (GPIO 7)
        .dc_gpio_num = DISP_DC_GPIO,              // Data/Command pin (GPIO 15)
        .spi_mode = 0,                            // SPI mode 0 (CPOL=0, CPHA=0)
        .pclk_hz = 40 * 1000 * 1000,              // 40 MHz clock frequency
        .trans_queue_depth = 10,                  // Queue up to 10 transfers
        .lcd_cmd_bits = 8,                        // Commands are 8-bit
        .lcd_param_bits = 8,                      // Parameters are 8-bit
        .on_color_trans_done = notify_flush_done, // ISR callback when DMA done
        .user_ctx = NULL,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SHARED_SPI_HOST, &io_config, &io_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "panel io init failed");
    ESP_LOGI(TAG, "SPI I/O interface created (CS=%d, DC=%d, 40MHz)", DISP_CS_GPIO, DISP_DC_GPIO);

    // ========================================================================
    // Configure ST7789 display controller
    // ========================================================================

    esp_lcd_panel_dev_config_t panel_config = {
    .reset_gpio_num = DISP_RST_GPIO,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
    .bits_per_pixel = 16,
};
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "panel st7789 init failed");
    ESP_LOGI(TAG, "ST7789 controller created (RST=%d, BGR, 16-bit)", DISP_RST_GPIO);

    // ========================================================================
    // Initialize display: reset, enable, configure orientation
    // ========================================================================

    // Hardware reset (RST pin pulse)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "reset failed");
    ESP_LOGI(TAG, "Display hardware reset complete");

    // Initialize the display (powers on, enters normal mode)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "init failed");
    ESP_LOGI(TAG, "Display initialized and powered on");

    // Invert colors if needed (fixes displays with inverted color bit order)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel_handle, DISP_INVERT_COLOR), TAG, "invert failed");

    // Swap X/Y axes for rotation
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, DISP_SWAP_XY), TAG, "swap_xy failed");

    // Mirror X and Y axes
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle, DISP_MIRROR_X, DISP_MIRROR_Y), TAG, "mirror failed");

    // Set display offset (for modules with memory gap vs. visible area)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel_handle, DISP_GAP_X, DISP_GAP_Y), TAG, "set_gap failed");

    // Turn on the display (exit sleep mode, enable output)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "disp_on failed");
    ESP_LOGI(TAG, "Display turned on (orientation: swap_xy=%d, mirror_x=%d, mirror_y=%d, invert=%d)",
             DISP_SWAP_XY, DISP_MIRROR_X, DISP_MIRROR_Y, DISP_INVERT_COLOR);

    // Initialize backlight
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");

    ESP_LOGI(TAG, "Display fully initialized and ready");
    return ESP_OK;
}

/**
 * Draw a rectangular region of RGB565 pixel data to the display.
 *
 * This function:
 * 1. Acquires the SPI bus mutex (blocking if another device is using it)
 * 2. Initiates a DMA transfer to the display
 * 3. Waits for the DMA transfer to complete (via ISR callback)
 * 4. Releases the SPI bus mutex
 *
 * Thread-safe: can be called from any task, even if SD card is also using the bus.
 *
 * @param x1, y1   Top-left corner (inclusive)
 * @param x2, y2   Bottom-right corner (exclusive)
 * @param color_data  RGB565 pixel buffer, (x2-x1)*(y2-y1) pixels.
 *                    Caller retains ownership; must not free/modify until
 *                    this function returns (DMA may still be in progress
 *                    when we return from esp_lcd_panel_draw_bitmap, so we wait
 *                    for the ISR callback before unlocking the bus).
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if display not initialized
 */
esp_err_t display_flush(int x1, int y1, int x2, int y2, const uint16_t *color_data)
{
    if (s_panel_handle == NULL) {
        ESP_LOGE(TAG, "display_flush: Display not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Acquire exclusive access to the SPI bus.
    // If SD card is using it, this will block until SD card releases it.
    spi_bus_lock();

    // Initiate DMA transfer to the display.
    // This returns quickly; the actual data transfer happens asynchronously via DMA,
    // and will trigger notify_flush_done() ISR when complete.
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel_handle, x1, y1, x2, y2, color_data);
    if (err == ESP_OK) {
        // Wait for the ISR to signal that DMA transfer is complete.
        // Only then is it safe to release the bus (and let SD card use it).
        xSemaphoreTake(s_flush_done_sem, portMAX_DELAY);
    } else {
        ESP_LOGE(TAG, "display_flush: draw_bitmap failed: %s", esp_err_to_name(err));
    }

    // Release the SPI bus for other devices (e.g., SD card)
    spi_bus_unlock();
    return err;
}

/**
 * Convenience function: fill the entire screen with a single RGB565 color.
 *
 * This allocates a framebuffer, fills it with the given color, calls display_flush(),
 * and then frees the buffer. Safe for demonstration and simple UI updates.
 *
 * @param color  RGB565 color value (e.g., 0xF800 for red)
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if framebuffer allocation fails
 *
 * Example:
 *   display_fill_color(0xF800);  // Red screen
 *   display_fill_color(0x07E0);  // Green screen
 *   display_fill_color(0x001F);  // Blue screen
 */
esp_err_t display_fill_color(uint16_t color)
{
    // Calculate total pixel count: 240 × 320 = 76,800 pixels
    // Each pixel is 2 bytes (RGB565), so total = 153,600 bytes
    size_t pixel_count = (size_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;

    // Allocate framebuffer in DMA-capable memory (external PSRAM if available)
    uint16_t *framebuffer = heap_caps_malloc(pixel_count * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (framebuffer == NULL) {
        ESP_LOGE(TAG, "display_fill_color: Failed to allocate %zu bytes", pixel_count * sizeof(uint16_t));
        return ESP_ERR_NO_MEM;
    }

    // Fill the entire framebuffer with the same color
    for (size_t i = 0; i < pixel_count; i++) {
        framebuffer[i] = color;
    }

    // Send the framebuffer to the display via DMA (this blocks until DMA completes)
    esp_err_t err = display_flush(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, framebuffer);

    // Safe to free now: display_flush() only returns AFTER the DMA transfer
    // has completed (see notify_flush_done() and the xSemaphoreTake in display_flush).
    free(framebuffer);

    if (err == ESP_OK) {
        ESP_LOGV(TAG, "display_fill_color: Screen filled with color 0x%04X", color);
    }
    return err;
}

