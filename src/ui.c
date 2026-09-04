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

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)
#define UI_QUEUE_LEN 10

// ============================================================================
// UI State
// ============================================================================

// Очередь событий интерфейса.
// Сюда input_bridge отправляет UI_EVT_UP, UI_EVT_DOWN и другие события.
static QueueHandle_t ui_queue = NULL;


// Все доступные экраны интерфейса.
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


// Текущий отображаемый экран.
static ui_screen_t current_screen = UI_SCREEN_SPLASH;


// ============================================================================
// Menu Types
// ============================================================================

// Callback вызывается при выборе пункта главного меню.
typedef void (*menu_cb_t)(void);


// Описание одного пункта меню.
typedef struct {
    const char *title;
    menu_cb_t callback;
} menu_item_t;


// ============================================================================
// Screen Actions
// ============================================================================

// Пока callbacks нужны только главному меню.
// Они переключают current_screen на соответствующий экран.

static void action_ir(void)
{
    ESP_LOGI(TAG, "Opened IR Remote");
    current_screen = UI_SCREEN_IR_MENU;
}

static void action_rf(void)
{
    ESP_LOGI(TAG, "Opened RF");
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

static void action_settings(void)
{
    ESP_LOGI(TAG, "Opened Settings");
    current_screen = UI_SCREEN_SETTINGS_MENU;
}


// ============================================================================
// Menu Data
// ============================================================================

// Главное меню устройства.
static const menu_item_t main_menu[] = {
    {"IR Remote",    action_ir},
    {"RF (Sub-GHz)", action_rf},
    {"NRF24",        action_nrf},
    {"Wi-Fi",        action_wifi},
    {"Bluetooth",    action_bt},
    {"Settings",     action_settings},
};


// Внутреннее меню IR.
static const char *ir_menu[] = {
    "TX / RX",
    "Saved Signals",
    "Protocols",
    "< BACK",
};


// Внутреннее меню Wi-Fi.
static const char *wifi_menu[] = {
    "Scan",
    "Networks",
    "Settings",
    "< BACK",
};


// Пока временное меню RF.
static const char *rf_menu[] = {
    "Receiver",
    "Transmitter",
    "Saved Signals",
    "< BACK",
};


// Пока временное меню NRF24.
static const char *nrf_menu[] = {
    "Receiver",
    "Transmitter",
    "Settings",
    "< BACK",
};


// Пока временное меню Bluetooth.
static const char *bt_menu[] = {
    "Scan",
    "Devices",
    "Settings",
    "< BACK",
};


// Пока временное меню Settings.
static const char *settings_menu[] = {
    "Display",
    "System",
    "About",
    "< BACK",
};


// ============================================================================
// Menu Counts
// ============================================================================

// Автоматически считаем количество элементов массива.
// Теперь не нужно вручную помнить, сколько пунктов в меню.

#define MENU_COUNT \
    (sizeof(main_menu) / sizeof(main_menu[0]))

#define IR_MENU_COUNT \
    (sizeof(ir_menu) / sizeof(ir_menu[0]))

#define WIFI_MENU_COUNT \
    (sizeof(wifi_menu) / sizeof(wifi_menu[0]))

#define RF_MENU_COUNT \
    (sizeof(rf_menu) / sizeof(rf_menu[0]))

#define NRF_MENU_COUNT \
    (sizeof(nrf_menu) / sizeof(nrf_menu[0]))

#define BLUETOOTH_MENU_COUNT \
    (sizeof(bt_menu) / sizeof(bt_menu[0]))

#define SETTINGS_MENU_COUNT \
    (sizeof(settings_menu) / sizeof(settings_menu[0]))


// ============================================================================
// Focus State
// ============================================================================

// Каждый экран хранит собственный текущий выбранный элемент.
//
// Это важно:
// если ты вышел из IR и потом вернулся,
// IR помнит, где последний раз находился фокус.

static int8_t main_selected = 0;
static int8_t ir_selected = 0;
static int8_t wifi_selected = 0;
static int8_t rf_selected = 0;
static int8_t nrf_selected = 0;
static int8_t bt_selected = 0;
static int8_t settings_selected = 0;


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

    // Прицел.
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
        cx - tick,
        cy + arm,
        0xFFFF
    );
}


// ============================================================================
// Focus Drawing
// ============================================================================

// Базовая функция первого уровня системы фокуса.
//
// focused = true:
//      [TEXT] белым цветом
//
// focused = false:
//      TEXT серым цветом
//
// Позже именно эту функцию можно расширить:
// добавить рамки, иконки, анимацию, курсор и другие типы объектов.

static void draw_focus_text(
    gfx_canvas_t *canvas,
    int16_t x,
    int16_t y,
    const char *text,
    bool focused
)
{
    if (focused) {

        char buffer[64];

        snprintf(
            buffer,
            sizeof(buffer),
            "[%s]",
            text
        );

        gfx_canvas_draw_str(
            canvas,
            x,
            y,
            buffer,
            UI_FONT,
            0xFFFF
        );

    } else {

        gfx_canvas_draw_str(
            canvas,
            x,
            y,
            text,
            UI_FONT,
            0x8410
        );
    }
}


// ============================================================================
// Focus Movement
// ============================================================================

// Первый уровень системы навигации.
//
// Пока реализована вертикальная навигация:
//
// UP   -> предыдущий элемент
// DOWN -> следующий элемент
//
// При выходе за границы меню происходит переход по кругу.

static void move_focus(
    int8_t *selected,
    int count,
    ui_event_t evt
)
{
    if (evt == UI_EVT_UP) {

        *selected =
            (*selected <= 0)
            ? count - 1
            : *selected - 1;
    }

    else if (evt == UI_EVT_DOWN) {

        *selected =
            (*selected >= count - 1)
            ? 0
            : *selected + 1;
    }
}


// ============================================================================
// Splash Screen
// ============================================================================

static void draw_splash_screen(gfx_canvas_t *canvas)
{
    uint16_t background =
        GFX_RGB565(0xF8, 0x00, 0x54);

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
// Generic Menu Drawing
// ============================================================================

// Универсальная отрисовка вертикального меню.
//
// title      - заголовок экрана
// items      - массив пунктов меню
// count      - количество пунктов
// selected   - текущий элемент в фокусе

static void draw_vertical_menu(
    gfx_canvas_t *canvas,
    const char *title,
    const char *const *items,
    int count,
    int8_t selected
)
{
    gfx_canvas_fill(canvas, 0x0000);

    // Верхняя разделительная линия.
    gfx_canvas_draw_line(
        canvas,
        0,
        18,
        DISPLAY_WIDTH - 1,
        18,
        0xFFFF
    );

    // Заголовок не является объектом фокуса.
    gfx_canvas_draw_str(
        canvas,
        10,
        9,
        title,
        UI_FONT,
        0xFFFF
    );

    const int16_t start_y = 45;
    const int16_t line_h = 20;

    for (int i = 0; i < count; i++) {

        int16_t y = start_y + (i * line_h);

        // Последний BACK немного отделяем визуально.
        if (i == count - 1) {
            y += 15;
        }

        draw_focus_text(
            canvas,
            10,
            y,
            items[i],
            i == selected
        );
    }
}


// ============================================================================
// Main Menu
// ============================================================================

static void draw_main_menu(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, 0x0000);

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

    for (int i = 0; i < MENU_COUNT; i++) {

        int16_t y = start_y + (i * line_h);

        // Старая механика if(active) удалена.
        // Теперь весь фокус проходит через draw_focus_text().
        draw_focus_text(
            canvas,
            10,
            y,
            main_menu[i].title,
            i == main_selected
        );
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
    draw_vertical_menu(
        canvas,
        "IR Remote",
        ir_menu,
        IR_MENU_COUNT,
        ir_selected
    );
}


// ============================================================================
// Wi-Fi Menu
// ============================================================================

static void draw_wifi_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "Wi-Fi",
        wifi_menu,
        WIFI_MENU_COUNT,
        wifi_selected
    );
}


// ============================================================================
// RF Menu
// ============================================================================

static void draw_rf_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "RF (Sub-GHz)",
        rf_menu,
        RF_MENU_COUNT,
        rf_selected
    );
}


// ============================================================================
// NRF24 Menu
// ============================================================================

static void draw_nrf_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "NRF24",
        nrf_menu,
        NRF_MENU_COUNT,
        nrf_selected
    );
}


// ============================================================================
// Bluetooth Menu
// ============================================================================

static void draw_bt_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "Bluetooth",
        bt_menu,
        BLUETOOTH_MENU_COUNT,
        bt_selected
    );
}


// ============================================================================
// Settings Menu
// ============================================================================

static void draw_settings_menu(gfx_canvas_t *canvas)
{
    draw_vertical_menu(
        canvas,
        "Settings",
        settings_menu,
        SETTINGS_MENU_COUNT,
        settings_selected
    );
}


// ============================================================================
// Render
// ============================================================================

// Единственная точка полной отрисовки интерфейса.
//
// Сначала определяется current_screen,
// затем вызывается функция отрисовки конкретного экрана,
// после чего canvas отправляется на дисплей.

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

        case UI_SCREEN_RF_MENU:
            draw_rf_menu(canvas);
            break;

        case UI_SCREEN_NRF_MENU:
            draw_nrf_menu(canvas);
            break;

        case UI_SCREEN_WIFI_MENU:
            draw_wifi_menu(canvas);
            break;

        case UI_SCREEN_BT_MENU:
            draw_bt_menu(canvas);
            break;

        case UI_SCREEN_SETTINGS_MENU:
            draw_settings_menu(canvas);
            break;

        default:
            break;
    }

    gfx_canvas_flush(canvas);
}


// ============================================================================
// Main Menu Event Handler
// ============================================================================

static void handle_main_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &main_selected,
                MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (main_menu[main_selected].callback) {

                main_menu[main_selected].callback();

                ui_render(canvas);
            }

            break;


        // LEFT / RIGHT пока зарезервированы под пространственный фокус.
        case UI_EVT_LEFT:
        case UI_EVT_RIGHT:

            ESP_LOGI(TAG, "Main menu horizontal navigation");

            break;


        default:
            break;
    }
}


// ============================================================================
// IR Event Handler
// ============================================================================

static void handle_ir_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &ir_selected,
                IR_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            // Последний пункт любого меню — BACK.
            if (ir_selected == IR_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        case UI_EVT_LEFT:
        case UI_EVT_RIGHT:

            ESP_LOGI(TAG, "IR horizontal navigation");

            break;


        default:
            break;
    }
}


// ============================================================================
// Wi-Fi Event Handler
// ============================================================================

static void handle_wifi_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &wifi_selected,
                WIFI_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (wifi_selected == WIFI_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        case UI_EVT_LEFT:
        case UI_EVT_RIGHT:

            ESP_LOGI(TAG, "Wi-Fi horizontal navigation");

            break;


        default:
            break;
    }
}


// ============================================================================
// RF Event Handler
// ============================================================================

static void handle_rf_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &rf_selected,
                RF_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (rf_selected == RF_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        default:
            break;
    }
}


// ============================================================================
// NRF24 Event Handler
// ============================================================================

static void handle_nrf_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &nrf_selected,
                NRF_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (nrf_selected == NRF_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        default:
            break;
    }
}


// ============================================================================
// Bluetooth Event Handler
// ============================================================================

static void handle_bt_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &bt_selected,
                BLUETOOTH_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (bt_selected == BLUETOOTH_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        default:
            break;
    }
}


// ============================================================================
// Settings Event Handler
// ============================================================================

static void handle_settings_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            move_focus(
                &settings_selected,
                SETTINGS_MENU_COUNT,
                evt
            );

            ui_render(canvas);

            break;


        case UI_EVT_SELECT:

            if (settings_selected == SETTINGS_MENU_COUNT - 1) {

                current_screen = UI_SCREEN_MAIN_MENU;

                ui_render(canvas);
            }

            break;


        default:
            break;
    }
}


// ============================================================================
// UI Event Router
// ============================================================================

// Центральный маршрутизатор событий.
//
// Здесь определяется:
// какой экран сейчас активен
// и какому обработчику передать событие.

static void ui_handle_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (current_screen) {

        case UI_SCREEN_MAIN_MENU:
            handle_main_menu_event(evt, canvas);
            break;

        case UI_SCREEN_IR_MENU:
            handle_ir_menu_event(evt, canvas);
            break;

        case UI_SCREEN_RF_MENU:
            handle_rf_menu_event(evt, canvas);
            break;

        case UI_SCREEN_NRF_MENU:
            handle_nrf_menu_event(evt, canvas);
            break;

        case UI_SCREEN_WIFI_MENU:
            handle_wifi_menu_event(evt, canvas);
            break;

        case UI_SCREEN_BT_MENU:
            handle_bt_menu_event(evt, canvas);
            break;

        case UI_SCREEN_SETTINGS_MENU:
            handle_settings_menu_event(evt, canvas);
            break;

        default:
            break;
    }
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
    // Инициализация canvas
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
    // Splash Screen
    // ------------------------------------------------------------------------

    current_screen = UI_SCREEN_SPLASH;

    ui_render(&canvas);

    vTaskDelay(pdMS_TO_TICKS(2000));


    // ------------------------------------------------------------------------
    // Main Menu
    // ------------------------------------------------------------------------

    current_screen = UI_SCREEN_MAIN_MENU;

    ui_render(&canvas);


    // ------------------------------------------------------------------------
    // Main Event Loop
    // ------------------------------------------------------------------------

    ui_event_t evt;

    while (1) {

        // Ждём событие от input_bridge.
        if (xQueueReceive(
                ui_queue,
                &evt,
                portMAX_DELAY
            ) != pdTRUE) {

            continue;
        }

        // Больше нет старого ограничения:
        //
        // if (current_screen != UI_SCREEN_MAIN_MENU)
        //     continue;
        //
        // Теперь каждое событие передаётся активному экрану.

        ui_handle_event(
            evt,
            &canvas
        );
    }


    // Теоретически сюда выполнение никогда не дойдёт.

    gfx_canvas_deinit(&canvas);

    vTaskDelete(NULL);
}
