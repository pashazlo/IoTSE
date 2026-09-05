#pragma once

#ifdef __cplusplus
extern "C" {
#endif

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

void ui_screen_set(ui_screen_t screen);
ui_screen_t ui_screen_get(void);

#ifdef __cplusplus
}
#endif
