#include "ui_controller.h"

#include "ui_screen.h"
#include "ui_menu.h"
#include "ui_focus.h"
#include "ui_render.h"

#include "esp_log.h"

static const char *TAG = "UI_CONTROLLER";


static void handle_main_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            ui_focus_move(
                UI_FOCUS_MAIN,
                main_menu_count,
                evt
            );

            ui_render(canvas);

            break;

        case UI_EVT_SELECT:
        {
            uint8_t selected = ui_focus_get(UI_FOCUS_MAIN);

            if (main_menu[selected].callback) {
                main_menu[selected].callback();
                ui_render(canvas);
            }

            break;
        }

        case UI_EVT_LEFT:
        case UI_EVT_RIGHT:

            ESP_LOGI(
                TAG,
                "Main menu horizontal navigation"
            );

            break;

        default:
            break;
    }
}


static void handle_ir_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            ui_focus_move(
                UI_FOCUS_IR,
                ir_menu_count,
                evt
            );

            ui_render(canvas);

            break;

        case UI_EVT_SELECT:

            if (ui_focus_get(UI_FOCUS_IR) == ir_menu_count - 1) {
                ui_screen_set(UI_SCREEN_MAIN_MENU);
                ui_render(canvas);
            }

            break;

        case UI_EVT_LEFT:
        case UI_EVT_RIGHT:

            ESP_LOGI(
                TAG,
                "IR horizontal navigation"
            );

            break;

        default:
            break;
    }
}


static void handle_wifi_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            ui_focus_move(
                UI_FOCUS_WIFI,
                wifi_menu_count,
                evt
            );

            ui_render(canvas);

            break;

        case UI_EVT_SELECT:

            if (ui_focus_get(UI_FOCUS_WIFI) == wifi_menu_count - 1) {
                ui_screen_set(UI_SCREEN_MAIN_MENU);
                ui_render(canvas);
            }

            break;

        case UI_EVT_LEFT:
        case UI_EVT_RIGHT:

            ESP_LOGI(
                TAG,
                "Wi-Fi horizontal navigation"
            );

            break;

        default:
            break;
    }
}


static void handle_rf_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            ui_focus_move(
                UI_FOCUS_RF,
                rf_menu_count,
                evt
            );

            ui_render(canvas);

            break;

        case UI_EVT_SELECT:

            if (ui_focus_get(UI_FOCUS_RF) == rf_menu_count - 1) {
                ui_screen_set(UI_SCREEN_MAIN_MENU);
                ui_render(canvas);
            }

            break;

        default:
            break;
    }
}


static void handle_nrf_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            ui_focus_move(
                UI_FOCUS_NRF,
                nrf_menu_count,
                evt
            );

            ui_render(canvas);

            break;

        case UI_EVT_SELECT:

            if (ui_focus_get(UI_FOCUS_NRF) == nrf_menu_count - 1) {
                ui_screen_set(UI_SCREEN_MAIN_MENU);
                ui_render(canvas);
            }

            break;

        default:
            break;
    }
}


static void handle_bt_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            ui_focus_move(
                UI_FOCUS_BT,
                bt_menu_count,
                evt
            );

            ui_render(canvas);

            break;

        case UI_EVT_SELECT:

            if (ui_focus_get(UI_FOCUS_BT) == bt_menu_count - 1) {
                ui_screen_set(UI_SCREEN_MAIN_MENU);
                ui_render(canvas);
            }

            break;

        default:
            break;
    }
}


static void handle_settings_menu_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:

            ui_focus_move(
                UI_FOCUS_SETTINGS,
                settings_menu_count,
                evt
            );

            ui_render(canvas);

            break;

        case UI_EVT_SELECT:

            if (ui_focus_get(UI_FOCUS_SETTINGS) == settings_menu_count - 1) {
                ui_screen_set(UI_SCREEN_MAIN_MENU);
                ui_render(canvas);
            }

            break;

        default:
            break;
    }
}


void ui_controller_handle_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    switch (ui_screen_get()) {

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
