#include "ui_menu.h"

#include "ui_screen.h"

// ============================================================================
// Данные меню — теперь ВСЕ пункты одного типа (menu_item_t), включая
// "< BACK". Массивы стали static: снаружи ui_menu.c их видеть не
// нужно — и controller, и render получают доступ только через
// ui_menu_get_screen() ниже. Это единственный файл, который трогаешь,
// когда добавляешь новый экран-меню.
// ============================================================================

static const menu_item_t main_menu[] = {
    {"IR Remote",    action_ir},
    {"RF (Sub-GHz)", action_rf},
    {"NRF24",        action_nrf},
    {"Wi-Fi",        action_wifi},
    {"Bluetooth",    action_bt},
    {"Settings",     action_settings},
};

static const menu_item_t ir_menu[] = {
    {"TX / RX",       NULL},
    {"Saved Signals", NULL},
    {"Protocols",     NULL},
    {"< BACK",        action_back_to_main},
};

static const menu_item_t wifi_menu[] = {
    {"Scan",     NULL},
    {"Networks", NULL},
    {"Settings", NULL},
    {"< BACK",   action_back_to_main},
};

static const menu_item_t rf_menu[] = {
    {"Receiver",      NULL},
    {"Transmitter",   NULL},
    {"Saved Signals", NULL},
    {"< BACK",        action_back_to_main},
};

static const menu_item_t nrf_menu[] = {
    {"Receiver",    NULL},
    {"Transmitter", NULL},
    {"Settings",    NULL},
    {"< BACK",      action_back_to_main},
};

static const menu_item_t bt_menu[] = {
    {"Scan",     NULL},
    {"Devices",  NULL},
    {"Settings", NULL},
    {"< BACK",   action_back_to_main},
};

static const menu_item_t settings_menu[] = {
    {"Display", NULL},
    {"System",  NULL},
    {"About",   NULL},
    {"< BACK",  action_back_to_main},
};


// ============================================================================
// Таблица-регистр: экран -> его меню
// ============================================================================

// Индексируется прямо значением ui_screen_t (designated initializers).
// UI_SCREEN_SPLASH не заполнен намеренно — остаётся нулевым
// (focus_id=0, header=NULL, items=NULL, count=0), и ui_menu_get_screen()
// явно возвращает для него NULL, а не эти нулевые данные.
static const ui_menu_screen_t s_screens[UI_SCREEN_COUNT] = {

    [UI_SCREEN_MAIN_MENU] = {
        .focus_id = UI_FOCUS_MAIN,
        .header   = NULL,   // у главного меню вместо заголовка часы (см. ui_render.c)
        .items    = main_menu,
        .count    = sizeof(main_menu) / sizeof(main_menu[0]),
    },

    [UI_SCREEN_IR_MENU] = {
        .focus_id = UI_FOCUS_IR,
        .header   = "IR Remote",
        .items    = ir_menu,
        .count    = sizeof(ir_menu) / sizeof(ir_menu[0]),
    },

    [UI_SCREEN_RF_MENU] = {
        .focus_id = UI_FOCUS_RF,
        .header   = "RF",
        .items    = rf_menu,
        .count    = sizeof(rf_menu) / sizeof(rf_menu[0]),
    },

    [UI_SCREEN_NRF_MENU] = {
        .focus_id = UI_FOCUS_NRF,
        .header   = "NRF24",
        .items    = nrf_menu,
        .count    = sizeof(nrf_menu) / sizeof(nrf_menu[0]),
    },

    [UI_SCREEN_WIFI_MENU] = {
        .focus_id = UI_FOCUS_WIFI,
        .header   = "Wi-Fi",
        .items    = wifi_menu,
        .count    = sizeof(wifi_menu) / sizeof(wifi_menu[0]),
    },

    [UI_SCREEN_BT_MENU] = {
        .focus_id = UI_FOCUS_BT,
        .header   = "Bluetooth",
        .items    = bt_menu,
        .count    = sizeof(bt_menu) / sizeof(bt_menu[0]),
    },

    [UI_SCREEN_SETTINGS_MENU] = {
        .focus_id = UI_FOCUS_SETTINGS,
        .header   = "Settings",
        .items    = settings_menu,
        .count    = sizeof(settings_menu) / sizeof(settings_menu[0]),
    },
};


const ui_menu_screen_t *ui_menu_get_screen(ui_screen_t screen)
{
    if (screen == UI_SCREEN_SPLASH || screen >= UI_SCREEN_COUNT) {
        return NULL;
    }

    return &s_screens[screen];
}
