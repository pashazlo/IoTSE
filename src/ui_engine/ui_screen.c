#include "ui_screen.h"

#include "esp_log.h"

static const char *TAG = "UI_SCREEN";

static ui_screen_t current_screen = UI_SCREEN_SPLASH;

void ui_screen_set(ui_screen_t screen)
{
    current_screen = screen;
}

ui_screen_t ui_screen_get(void)
{
    return current_screen;
}

void action_ir(void)
{
    ESP_LOGI(TAG, "Opened IR Remote");
    current_screen = UI_SCREEN_IR_MENU;
}

void action_rf(void)
{
    ESP_LOGI(TAG, "Opened RF");
    current_screen = UI_SCREEN_RF_MENU;
}

void action_nrf(void)
{
    ESP_LOGI(TAG, "Opened NRF24");
    current_screen = UI_SCREEN_NRF_MENU;
}

void action_wifi(void)
{
    ESP_LOGI(TAG, "Opened Wi-Fi");
    current_screen = UI_SCREEN_WIFI_MENU;
}

void action_bt(void)
{
    ESP_LOGI(TAG, "Opened Bluetooth");
    current_screen = UI_SCREEN_BT_MENU;
}

void action_settings(void)
{
    ESP_LOGI(TAG, "Opened Settings");
    current_screen = UI_SCREEN_SETTINGS_MENU;
}

void action_back_to_main(void)
{
    ESP_LOGI(TAG, "Back to main menu");
    current_screen = UI_SCREEN_MAIN_MENU;
}
