#include "ui_screen.h"
#include "esp_log.h"
#include "ui_keyboard.h"

static const char *TAG = "UI_SCREEN";

// Текущее состояние экрана приложения
static ui_screen_t current_screen = UI_SCREEN_SPLASH;

// ----------------------------------------------------------------------------
// Базовый API получения и установки экрана
// ----------------------------------------------------------------------------
void ui_screen_set(ui_screen_t screen)
{
    current_screen = screen;
}

ui_screen_t ui_screen_get(void)
{
    return current_screen;
}

// ----------------------------------------------------------------------------
// Переходы в меню периферии и модулей
// ----------------------------------------------------------------------------
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

// ----------------------------------------------------------------------------
// Файловая система и Редактор Текста
// ----------------------------------------------------------------------------

// Вызов файлового менеджера (просмотр разделов / SD-карты)
void action_open_file_manager(void)
{
    ESP_LOGI(TAG, "Opened File Manager");
    current_screen = UI_SCREEN_FILE_VOLUMES;
}

// Открытие текстового редактора
// (Вызывается при выборе .txt / .c файла в файловом менеджере)
void action_open_file_editor(void)
{
    ESP_LOGI(TAG, "Opened File Editor");
    current_screen = UI_SCREEN_FILE_EDITOR;
}

// Открытие модальной клавиатуры 
// (Клавиатура рисуется поверх текущего экрана, не меняя current_screen)
void action_open_keyboard(const char *initial_text)
{
    ESP_LOGI(TAG, "Opening On-Screen Keyboard");
    ui_keyboard_open(initial_text);
}
