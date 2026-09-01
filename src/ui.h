#pragma once
#define DISPLAY_WIDTH  320
#define DISPLAY_HEIGHT 240
#ifdef __cplusplus
extern "C" {
#endif

// Задача интерфейса. Запускается из main.
void ui_task(void *arg);

#ifdef __cplusplus
}
#endif
