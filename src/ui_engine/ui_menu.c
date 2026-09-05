#include "ui_menu.h"

#include "ui_screen.h"

const menu_item_t main_menu[] = {
    {"IR Remote",    action_ir},
    {"RF (Sub-GHz)", action_rf},
    {"NRF24",        action_nrf},
    {"Wi-Fi",        action_wifi},
    {"Bluetooth",    action_bt},
    {"Settings",     action_settings},
};

const uint8_t main_menu_count =
    sizeof(main_menu) / sizeof(main_menu[0]);


const char *ir_menu[] = {
    "TX / RX",
    "Saved Signals",
    "Protocols",
    "< BACK",
};

const uint8_t ir_menu_count =
    sizeof(ir_menu) / sizeof(ir_menu[0]);


const char *wifi_menu[] = {
    "Scan",
    "Networks",
    "Settings",
    "< BACK",
};

const uint8_t wifi_menu_count =
    sizeof(wifi_menu) / sizeof(wifi_menu[0]);


const char *rf_menu[] = {
    "Receiver",
    "Transmitter",
    "Saved Signals",
    "< BACK",
};

const uint8_t rf_menu_count =
    sizeof(rf_menu) / sizeof(rf_menu[0]);


const char *nrf_menu[] = {
    "Receiver",
    "Transmitter",
    "Settings",
    "< BACK",
};

const uint8_t nrf_menu_count =
    sizeof(nrf_menu) / sizeof(nrf_menu[0]);


const char *bt_menu[] = {
    "Scan",
    "Devices",
    "Settings",
    "< BACK",
};

const uint8_t bt_menu_count =
    sizeof(bt_menu) / sizeof(bt_menu[0]);


const char *settings_menu[] = {
    "Display",
    "System",
    "About",
    "< BACK",
};

const uint8_t settings_menu_count =
    sizeof(settings_menu) / sizeof(settings_menu[0]);
