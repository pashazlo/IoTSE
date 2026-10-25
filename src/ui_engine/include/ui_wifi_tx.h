#pragma once

/* =====================================================================
 *  UI: экран «TX ESP» — настройки пакета (пункт меню Wi-Fi)
 * =====================================================================
 *
 * Это ОТДЕЛЬНЫЙ экран главного меню (Wi-Fi → TX ESP), НЕ часть Air Monitor.
 * Он показывает и (позже) позволяет менять параметры передачи, которые хранит
 * модель tx_config: SSID маяка, deauth reason, размер burst и канал.
 *
 * Паттерн тот же, что у других кастомных экранов (Export PCAP, Air Monitor):
 *   open()         — сделать его текущим экраном и сбросить фокус;
 *   handle_event() — обработать кнопки (навигация, выход);
 *   draw()         — нарисовать кадр.
 * Регистрация экрана — в ui_menu.c (пункт), ui_screen.c (action), ui_controller.c
 * (события) и ui_render.c (отрисовка). */

#include "gfx_canvas.h"
#include "ui_cursor.h"
#include "ui_event.h"

void ui_wifi_tx_open(void);
void ui_wifi_tx_handle_event(ui_event_t event, gfx_canvas_t *canvas);
void ui_wifi_tx_draw(gfx_canvas_t *canvas, ui_cursor_t *cursor);

/* true, пока длинное значение прокручивается — render продолжает выдавать кадры
 * без новых нажатий кнопок (как у других экранов со скроллом текста). */
bool ui_wifi_tx_needs_tick(void);

/* Применить результат модальной клавиатуры к SSID. Контроллер зовёт это после
 * закрытия клавиатуры, когда текущий экран — TX ESP. */
void ui_wifi_tx_handle_keyboard_result(void);
