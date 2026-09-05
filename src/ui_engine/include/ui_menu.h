#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*menu_cb_t)(void);

typedef struct {
    const char *title;
    menu_cb_t callback;
} menu_item_t;

extern const menu_item_t main_menu[];
extern const uint8_t main_menu_count;

extern const char *ir_menu[];
extern const uint8_t ir_menu_count;

extern const char *wifi_menu[];
extern const uint8_t wifi_menu_count;

extern const char *rf_menu[];
extern const uint8_t rf_menu_count;

extern const char *nrf_menu[];
extern const uint8_t nrf_menu_count;

extern const char *bt_menu[];
extern const uint8_t bt_menu_count;

extern const char *settings_menu[];
extern const uint8_t settings_menu_count;

#ifdef __cplusplus
}
#endif
