#include "ui_cursor.h"

#include <string.h>
#include <math.h>

// Скорость "подъезда" — доля ОСТАВШЕГОСЯ расстояния, которую курсор
// проезжает за один шаг анимации (0..1). Больше значение — быстрее
// и резче; меньше — плавнее и медленнее. 0.35 — хороший компромисс
// для меню на 20px строках.
#define UI_CURSOR_LERP_SPEED   0.35f

// Порог в пикселях, ближе которого считаем, что курсор "доехал" до
// цели по всем 4 параметрам (x, y, w, h), и останавливаем анимацию.
// Без этого lerp приближался бы к цели бесконечно, никогда не достигая
// ровно 0.
#define UI_CURSOR_SNAP_EPS     0.5f

// Длина одного "уса" уголка-скобки в пикселях.
#define UI_CURSOR_CORNER_LEN   5


void ui_cursor_reset(ui_cursor_t *cur)
{
    memset(cur, 0, sizeof(*cur));
}


void ui_cursor_set_target(ui_cursor_t *cur, int16_t x, int16_t y, int16_t w, int16_t h)
{
    if (!cur->initialized) {
        // Первый показ курсора — сразу появляется на месте,
        // не летит через весь экран из точки (0,0).
        cur->x = (float)x;
        cur->y = (float)y;
        cur->w = (float)w;
        cur->h = (float)h;
        cur->initialized = true;
        cur->animating = false;

    } else if ((float)x != cur->target_x || (float)y != cur->target_y ||
               (float)w != cur->target_w || (float)h != cur->target_h) {
        // Цель реально сдвинулась относительно прошлого кадра —
        // включаем анимацию. Если target тот же самый (например,
        // экран просто перерисовался раз в секунду ради часов),
        // анимацию заново не запускаем.
        cur->animating = true;
    }

    cur->target_x = (float)x;
    cur->target_y = (float)y;
    cur->target_w = (float)w;
    cur->target_h = (float)h;
}


void ui_cursor_step(ui_cursor_t *cur)
{
    if (!cur->animating) {
        return;
    }

    cur->x += (cur->target_x - cur->x) * UI_CURSOR_LERP_SPEED;
    cur->y += (cur->target_y - cur->y) * UI_CURSOR_LERP_SPEED;
    cur->w += (cur->target_w - cur->w) * UI_CURSOR_LERP_SPEED;
    cur->h += (cur->target_h - cur->h) * UI_CURSOR_LERP_SPEED;

    float dx = cur->target_x - cur->x;
    float dy = cur->target_y - cur->y;
    float dw = cur->target_w - cur->w;
    float dh = cur->target_h - cur->h;

    if (fabsf(dx) < UI_CURSOR_SNAP_EPS && fabsf(dy) < UI_CURSOR_SNAP_EPS &&
        fabsf(dw) < UI_CURSOR_SNAP_EPS && fabsf(dh) < UI_CURSOR_SNAP_EPS) {
        // Долетели — прилипаем ровно к цели и выключаем анимацию.
        cur->x = cur->target_x;
        cur->y = cur->target_y;
        cur->w = cur->target_w;
        cur->h = cur->target_h;
        cur->animating = false;
    }
}


void ui_cursor_draw(gfx_canvas_t *canvas, const ui_cursor_t *cur, uint16_t color)
{
    if (!cur->initialized) {
        return;
    }

    int16_t x0 = (int16_t)cur->x;
    int16_t y0 = (int16_t)cur->y;
    int16_t x1 = (int16_t)(cur->x + cur->w);
    int16_t y1 = (int16_t)(cur->y + cur->h);
    int16_t len = UI_CURSOR_CORNER_LEN;

    // Верхний левый уголок
    gfx_canvas_draw_line(canvas, x0, y0, x0 + len, y0, color);
    gfx_canvas_draw_line(canvas, x0, y0, x0, y0 + len, color);

    // Верхний правый уголок
    gfx_canvas_draw_line(canvas, x1, y0, x1 - len, y0, color);
    gfx_canvas_draw_line(canvas, x1, y0, x1, y0 + len, color);

    // Нижний левый уголок
    gfx_canvas_draw_line(canvas, x0, y1, x0 + len, y1, color);
    gfx_canvas_draw_line(canvas, x0, y1, x0, y1 - len, color);

    // Нижний правый уголок
    gfx_canvas_draw_line(canvas, x1, y1, x1 - len, y1, color);
    gfx_canvas_draw_line(canvas, x1, y1, x1, y1 - len, color);
}
