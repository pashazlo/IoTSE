#include "ui.h"

#include "display.h"
#include "gfx_canvas.h"
#include "assets/ibm_vga_font.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"


// ============================================================================
// Configuration
// ============================================================================

static const char *TAG = "UI";

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b7)

#define UI_QUEUE_LEN 10


// ============================================================================
// UI State
// ============================================================================

static QueueHandle_t ui_queue = NULL;


typedef enum {
    UI_SCREEN_SPLASH,
    UI_SCREEN_MAIN_MENU,

    UI_SCREEN_IR_MENU,
    UI_SCREEN_RF_MENU,
    UI_SCREEN_NRF_MENU,
    UI_SCREEN_WIFI_MENU,
    UI_SCREEN_BT_MENU,
    UI_SCREEN_SETTINGS_MENU,

} ui_screen_t;


static ui_screen_t current_screen = UI_SCREEN_SPLASH;


// ============================================================================
// Menu
// ============================================================================

typedef void (*menu_cb_t)(void);


typedef struct {
    const char *title;
    menu_cb_t callback;
} menu_item_t;


// ============================================================================
// Menu Actions
// ============================================================================

static void action_ir(void)
{
    ESP_LOGI(TAG, "Opened IR");
    current_screen = UI_SCREEN_IR_MENU;
}


static void action_settings(void)
{
    ESP_LOGI(TAG, "Opened Settings");
    current_screen = UI_SCREEN_SETTINGS_MENU;
}


static void action_rf(void)
{
    ESP_LOGI(TAG, "Opened RF (Sub-GHz)");
    current_screen = UI_SCREEN_RF_MENU;
}


static void action_nrf(void)
{
    ESP_LOGI(TAG, "Opened NRF24");
    current_screen = UI_SCREEN_NRF_MENU;
}


static void action_wifi(void)
{
    ESP_LOGI(TAG, "Opened Wi-Fi");
    current_screen = UI_SCREEN_WIFI_MENU;
}


static void action_bt(void)
{
    ESP_LOGI(TAG, "Opened Bluetooth");
    current_screen = UI_SCREEN_BT_MENU;
}


// ============================================================================
// Main Menu
// ============================================================================

static const menu_item_t main_menu[] = {
    {"IR Remote",    action_ir},
    {"RF (Sub-GHz)", action_rf},
    {"NRF24",        action_nrf},
    {"Wi-Fi",        action_wifi},
    {"Bluetooth",    action_bt},
    {"Settings",     action_settings},
};


#define MENU_COUNT (sizeof(main_menu) / sizeof(main_menu[0]))


// Currently selected menu item
static int8_t current_selected = 0;


// ============================================================================
// Red Team Logo
// ============================================================================

static void draw_redteam_logo(
    gfx_canvas_t *canvas,
    int16_t cx,
    int16_t cy,
    uint16_t bg_color
)
{
    gfx_canvas_draw_circle(canvas, cx, cy, 42, 0xFFFF);
    gfx_canvas_draw_circle(canvas, cx, cy, 41, 0xFFFF);

    gfx_canvas_draw_circle(canvas, cx, cy, 26, 0xFFFF);

    gfx_canvas_fill_circle(canvas, cx, cy, 11, 0xFFFF);

    gfx_canvas_fill_circle(
        canvas,
        cx - 5,
        cy - 5,
        3,
        bg_color
    );


    // Crosshair

    const int16_t arm = 68;
    const int16_t gap = 46;
    const int16_t tick = 4;


    gfx_canvas_draw_line(
        canvas,
        cx - arm,
        cy,
        cx - gap,
        cy,
        0xFFFF
    );


    gfx_canvas_draw_line(
        canvas,
        cx + gap,
        cy,
        cx + arm,
        cy,
        0xFFFF
    );


    gfx_canvas_draw_line(
        canvas,
        cx,
        cy - arm,
        cx,
        cy - gap,
        0xFFFF
    );


    gfx_canvas_draw_line(
        canvas,
        cx,
        cy + gap,
        cx,
        cy + arm,
        0xFFFF
    );


    gfx_canvas_draw_line(
        canvas,
        cx - arm,
        cy - tick,
        cx - arm,
        cy + tick,
        0xFFFF
    );


    gfx_canvas_draw_line(
        canvas,
        cx + arm,
        cy - tick,
        cx + arm,
        cy + tick,
        0xFFFF
    );


    gfx_canvas_draw_line(
        canvas,
        cx - tick,
        cy - arm,
        cx + tick,
        cy - arm,
        0xFFFF
    );


    gfx_canvas_draw_line(
        canvas,
        cx - tick,
        cy + arm,
        cx + tick,
        cy + arm,
        0xFFFF
    );
}


// ============================================================================
// Splash Screen
// ============================================================================

static void draw_splash_screen(gfx_canvas_t *canvas)
{
    uint16_t background = GFX_RGB565(0xF8, 0x00, 0x54);


    gfx_canvas_fill(canvas, background);


    gfx_canvas_draw_line(
        canvas,
        0,
        12,
        DISPLAY_WIDTH - 1,
        12,
        0xFFFF
    );


    gfx_canvas_draw_line(
        canvas,
        0,
        DISPLAY_HEIGHT - 12,
        DISPLAY_WIDTH - 1,
        DISPLAY_HEIGHT - 12,
        0xFFFF
    );


    draw_redteam_logo(
        canvas,
        230,
        DISPLAY_HEIGHT / 2,
        background
    );
}


// ============================================================================
// Main Menu
// ============================================================================

static void draw_main_menu(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, 0x0000);


    // Top border

    gfx_canvas_draw_line(
        canvas,
        0,
        18,
        DISPLAY_WIDTH - 1,
        18,
        0xFFFF
    );


    const int16_t start_y = 40;
    const int16_t line_h = 20;


    for (uint8_t i = 0; i < MENU_COUNT; i++) {

        int16_t y = start_y + (i * line_h);

        bool active = (i == current_selected);


        if (active) {

            char buf[32];

            snprintf(
                buf,
                sizeof(buf),
                "[%s]",
                main_menu[i].title
            );


            gfx_canvas_draw_str(
                canvas,
                10,
                y,
                buf,
                UI_FONT,
                0xFFFF
            );

        } else {

            gfx_canvas_draw_str(
                canvas,
                10,
                y,
                main_menu[i].title,
                UI_FONT,
                0x8410
            );
        }
    }


    draw_redteam_logo(
        canvas,
        220,
        90,
        0x0000
    );
}


// ============================================================================
// IR Menu
// ============================================================================

static void draw_ir_menu(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, 0x0000);


    // Top border

    gfx_canvas_draw_line(
        canvas,
        0,
        18,
        DISPLAY_WIDTH - 1,
        18,
        0xFFFF
    );


    gfx_canvas_draw_str(
        canvas,
        10,
        40,
        "[IR Remote]",
        UI_FONT,
        0xFFFF
    );


    gfx_canvas_draw_str(
        canvas,
        10,
        65,
        "Прием / Передача",
        UI_FONT,
        0x8410
    );


    gfx_canvas_draw_str(
        canvas,
        10,
        85,
        "Записанные сигналы",
        UI_FONT,
        0x8410
    );


    gfx_canvas_draw_str(
        canvas,
        10,
        105,
        "Протоколы",
        UI_FONT,
        0x8410
    );


    // Back

    gfx_canvas_draw_str(
        canvas,
        10,
        140,
        "[< BACK]",
        UI_FONT,
        0x8410
    );
}


// ============================================================================
// Wi-Fi Menu
// ============================================================================

static void draw_wifi_menu(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, 0x0000);


    // Top border

    gfx_canvas_draw_line(
        canvas,
        0,
        18,
        DISPLAY_WIDTH - 1,
        18,
        0xFFFF
    );


    gfx_canvas_draw_str(
        canvas,
        10,
        40,
        "[Wi-Fi]",
        UI_FONT,
        0xFFFF
    );


    gfx_canvas_draw_str(
        canvas,
        10,
        65,
        "Сканирование",
        UI_FONT,
        0x8410
    );


    gfx_canvas_draw_str(
        canvas,
        10,
        85,
        "Сети",
        UI_FONT,
        0x8410
    );


    gfx_canvas_draw_str(
        canvas,
        10,
        105,
        "Настройки",
        UI_FONT,
        0x8410
    );


    // Back

    gfx_canvas_draw_str(
        canvas,
        10,
        140,
        "[< BACK]",
        UI_FONT,
        0x8410
    );
}


// ============================================================================
// Simple Module Screen
// ============================================================================

static void draw_simple_screen(
    gfx_canvas_t *canvas,
    const char *title
)
{
    gfx_canvas_fill(canvas, 0x0000);


    // Top border

    gfx_canvas_draw_line(
        canvas,
        0,
        18,
        DISPLAY_WIDTH - 1,
        18,
        0xFFFF
    );


    // Title

    gfx_canvas_draw_str(
        canvas,
        10,
        40,
        title,
        UI_FONT,
        0xFFFF
    );


    // Back

    gfx_canvas_draw_str(
        canvas,
        10,
        100,
        "[< BACK]",
        UI_FONT,
        0x8410
    );
}


// ============================================================================
// Render
// ============================================================================

static void ui_render(gfx_canvas_t *canvas)
{
    switch (current_screen) {

        case UI_SCREEN_SPLASH:

            draw_splash_screen(canvas);

            break;


        case UI_SCREEN_MAIN_MENU:

            draw_main_menu(canvas);

            break;


        case UI_SCREEN_IR_MENU:

            draw_ir_menu(canvas);

            break;


        case UI_SCREEN_WIFI_MENU:

            draw_wifi_menu(canvas);

            break;


        case UI_SCREEN_RF_MENU:

            draw_simple_screen(
                canvas,
                "[RF (Sub-GHz)]"
            );

            break;


        case UI_SCREEN_NRF_MENU:

            draw_simple_screen(
                canvas,
                "[NRF24]"
            );

            break;


        case UI_SCREEN_BT_MENU:

            draw_simple_screen(
                canvas,
                "[Bluetooth]"
            );

            break;


        case UI_SCREEN_SETTINGS_MENU:

            draw_simple_screen(
                canvas,
                "[Settings]"
            );

            break;


        default:

            break;
    }


    gfx_canvas_flush(canvas);
}


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
    gfx_canvas_t canvas;


    // ------------------------------------------------------------------------
    // Canvas initialization
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
    // Splash screen
    // ------------------------------------------------------------------------

    current_screen = UI_SCREEN_SPLASH;

    ui_render(&canvas);

    vTaskDelay(pdMS_TO_TICKS(2000));


    // ------------------------------------------------------------------------
    // Main menu
    // ------------------------------------------------------------------------

    current_screen = UI_SCREEN_MAIN_MENU;

    ui_render(&canvas);


    // ------------------------------------------------------------------------
    // Event loop
    // ------------------------------------------------------------------------

    ui_event_t evt;


    while (1) {

        if (xQueueReceive(
                ui_queue,
                &evt,
                portMAX_DELAY
            ) != pdTRUE) {

            continue;
        }


        // Пока обработка событий только главного меню

        if (current_screen != UI_SCREEN_MAIN_MENU) {
            continue;
        }


        switch (evt) {


            // ------------------------------------------------------------
            // UP
            // ------------------------------------------------------------

            case UI_EVT_UP:

                current_selected =
                    (current_selected <= 0)
                    ? MENU_COUNT - 1
                    : current_selected - 1;

                ui_render(&canvas);

                break;


            // ------------------------------------------------------------
            // DOWN
            // ------------------------------------------------------------

            case UI_EVT_DOWN:

                current_selected =
                    (current_selected >= (int8_t)MENU_COUNT - 1)
                    ? 0
                    : current_selected + 1;

                ui_render(&canvas);

                break;


            // ------------------------------------------------------------
            // LEFT
            // ------------------------------------------------------------

            case UI_EVT_LEFT:

                ESP_LOGI(TAG, "UI LEFT");

                break;


            // ------------------------------------------------------------
            // RIGHT
            // ------------------------------------------------------------

            case UI_EVT_RIGHT:

                ESP_LOGI(TAG, "UI RIGHT");

                break;


            // ------------------------------------------------------------
            // SELECT
            // ------------------------------------------------------------

            case UI_EVT_SELECT:

                if (main_menu[current_selected].callback) {

                    main_menu[current_selected].callback();

                    ui_render(&canvas);
                }

                break;


            // ------------------------------------------------------------
            // NONE / Unknown
            // ------------------------------------------------------------

            case UI_EVT_NONE:

            default:

                break;
        }
    }


    // Теоретически сюда код никогда не дойдёт.

    gfx_canvas_deinit(&canvas);

    vTaskDelete(NULL);
}
