#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*menu_cb_t)(void);

typedef struct {
    const char *title;
    menu_cb_t callback;
} menu_item_t;

extern const menu_item_t main_menu[];

extern const char *ir_menu[];
extern const char *wifi_menu[];
extern const char *rf_menu[];
extern const char *nrf_menu[];
extern const char *bt_menu[];
extern const char *settings_menu[];

#define MENU_COUNT (sizeof(main_menu) / sizeof(main_menu[0]))
#define IR_MENU_COUNT (sizeof(ir_menu) / sizeof(ir_menu[0]))
#define WIFI_MENU_COUNT (sizeof(wifi_menu) / sizeof(wifi_menu[0]))
#define RF_MENU_COUNT (sizeof(rf_menu) / sizeof(rf_menu[0]))
#define NRF_MENU_COUNT (sizeof(nrf_menu) / sizeof(nrf_menu[0]))
#define BLUETOOTH_MENU_COUNT (sizeof(bt_menu) / sizeof(bt_menu[0]))
#define SETTINGS_MENU_COUNT (sizeof(settings_menu) / sizeof(settings_menu[0]))

#ifdef __cplusplus
}
#endif
