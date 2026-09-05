#pragma once

#include <stdbool.h>
#include "gfx_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_render(gfx_canvas_t *canvas);

/**
 * @brief Возвращает true, пока курсор-рамка ещё "едет" к выбранному
 *        объекту. Используется в ui.c: пока идёт анимация, event loop
 *        временно тикает чаще (для плавности), в покое — редко (для
 *        экономии CPU/батареи), см. ui_task().
 */
bool ui_render_cursor_is_animating(void);

#ifdef __cplusplus
}
#endif
