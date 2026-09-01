#pragma once

#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_EVT_NONE = 0,
    UI_EVT_UP,
    UI_EVT_DOWN,
    UI_EVT_SELECT,
    UI_EVT_BACK
} ui_event_t;

// Задача UI
void ui_task(void *arg);

// Отправка события из обычных задач
BaseType_t ui_send_event(ui_event_t evt);

// Отправка события из обработчиков прерываний (ISR кнопок)
BaseType_t ui_send_event_from_isr(ui_event_t evt, BaseType_t *hp_task_woken);

#ifdef __cplusplus
}
#endif
