#include "ui_menu.h"

#include "ui_screen.h"
#include "esp_log.h"

static const char *TAG = "UI_MENU";

const menu_item_t main_menu[] = {
    {"IR Remote",    action_ir},
    {"RF (Sub-GHz)", action_rf},
    {"NRF24",        action_nrf},
    {"Wi-Fi",        action_wifi},
    {"Bluetooth",    action_bt},
    {"Settings",     action_settings},
};

const char *ir_menu[] = {
    "TX / RX",
    "Saved Signals",
    "Protocols",
    "< BACK",
};

const char *wifi_menu[] = {
    "Scan",
    "Networks",
    "Settings",
    "< BACK",
};

const char *rf_menu[] = {
    "Receiver",
    "Transmitter",
    "Saved Signals",
    "< BACK",
};

const char *nrf_menu[] = {
    "Receiver",
    "Transmitter",
    "Settings",
    "< BACK",
};

const char *bt_menu[] = {
    "Scan",
    "Devices",
    "Settings",
    "< BACK",
};

const char *settings_menu[] = {
    "Display",
    "System",
    "About",
    "< BACK",
};
