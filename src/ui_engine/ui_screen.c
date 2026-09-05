#include "ui_screen.h"

static ui_screen_t current_screen = UI_SCREEN_SPLASH;

void ui_screen_set(ui_screen_t screen)
{
    current_screen = screen;
}

ui_screen_t ui_screen_get(void)
{
    return current_screen;
}
