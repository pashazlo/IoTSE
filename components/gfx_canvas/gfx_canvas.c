#include "gfx_canvas.h"
#include "display.h"

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_check.h"

static const char *TAG = "gfx_canvas";

// ---- dirty rect tracking ----

static void mark_dirty(gfx_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->width) x1 = c->width;
    if (y1 > c->height) y1 = c->height;
    if (x0 >= x1 || y0 >= y1) return;

    if (c->dirty_x0 > c->dirty_x1) {
        c->dirty_x0 = x0; c->dirty_y0 = y0;
        c->dirty_x1 = x1; c->dirty_y1 = y1;
    } else {
        if (x0 < c->dirty_x0) c->dirty_x0 = x0;
        if (y0 < c->dirty_y0) c->dirty_y0 = y0;
        if (x1 > c->dirty_x1) c->dirty_x1 = x1;
        if (y1 > c->dirty_y1) c->dirty_y1 = y1;
    }
}

static void clear_dirty(gfx_canvas_t *c)
{
    c->dirty_x0 = c->dirty_y0 = 0;
    c->dirty_x1 = c->dirty_y1 = -1;
}

// ---- lifecycle ----

esp_err_t gfx_canvas_init(gfx_canvas_t *canvas, int16_t width, int16_t height)
{
    memset(canvas, 0, sizeof(*canvas));
    canvas->buf = heap_caps_malloc((size_t)width * height * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(canvas->buf != NULL, ESP_ERR_NO_MEM, TAG,
                         "canvas alloc failed (%dx%d = %d bytes)", width, height,
                         (int)(width * height * sizeof(uint16_t)));

    canvas->width = width;
    canvas->height = height;
    canvas->text_color = GFX_WHITE;
    canvas->text_bg_color = GFX_BLACK;
    canvas->text_bg_opaque = false;
    canvas->font = NULL;

    clear_dirty(canvas);
    memset(canvas->buf, 0, (size_t)width * height * sizeof(uint16_t));
    return ESP_OK;
}

void gfx_canvas_deinit(gfx_canvas_t *canvas)
{
    if (canvas->buf) {
        free(canvas->buf);
        canvas->buf = NULL;
    }
}

// ---- primitives ----

void gfx_canvas_fill(gfx_canvas_t *c, uint16_t color)
{
    size_t n = (size_t)c->width * c->height;
    for (size_t i = 0; i < n; i++) c->buf[i] = color;
    mark_dirty(c, 0, 0, c->width, c->height);
}

void gfx_canvas_draw_pixel(gfx_canvas_t *c, int16_t x, int16_t y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= c->width || y >= c->height) return;
    c->buf[(int32_t)y * c->width + x] = color;
    mark_dirty(c, x, y, x + 1, y + 1);
}

void gfx_canvas_fill_rect(gfx_canvas_t *c, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    int16_t x0 = x < 0 ? 0 : x;
    int16_t y0 = y < 0 ? 0 : y;
    int16_t x1 = (x + w > c->width) ? c->width : x + w;
    int16_t y1 = (y + h > c->height) ? c->height : y + h;
    for (int16_t yy = y0; yy < y1; yy++) {
        uint16_t *row = &c->buf[(int32_t)yy * c->width];
        for (int16_t xx = x0; xx < x1; xx++) row[xx] = color;
    }
    mark_dirty(c, x0, y0, x1, y1);
}

void gfx_canvas_draw_rect(gfx_canvas_t *c, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    gfx_canvas_fill_rect(c, x, y, w, 1, color);
    gfx_canvas_fill_rect(c, x, y + h - 1, w, 1, color);
    gfx_canvas_fill_rect(c, x, y, 1, h, color);
    gfx_canvas_fill_rect(c, x + w - 1, y, 1, h, color);
}

void gfx_canvas_draw_line(gfx_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    int16_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int16_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy;
    while (1) {
        gfx_canvas_draw_pixel(c, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int16_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_canvas_draw_circle(gfx_canvas_t *c, int16_t x0, int16_t y0, int16_t r, uint16_t color)
{
    int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    gfx_canvas_draw_pixel(c, x0, y0 + r, color);
    gfx_canvas_draw_pixel(c, x0, y0 - r, color);
    gfx_canvas_draw_pixel(c, x0 + r, y0, color);
    gfx_canvas_draw_pixel(c, x0 - r, y0, color);
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x;
        gfx_canvas_draw_pixel(c, x0 + x, y0 + y, color);
        gfx_canvas_draw_pixel(c, x0 - x, y0 + y, color);
        gfx_canvas_draw_pixel(c, x0 + x, y0 - y, color);
        gfx_canvas_draw_pixel(c, x0 - x, y0 - y, color);
        gfx_canvas_draw_pixel(c, x0 + y, y0 + x, color);
        gfx_canvas_draw_pixel(c, x0 - y, y0 + x, color);
        gfx_canvas_draw_pixel(c, x0 + y, y0 - x, color);
        gfx_canvas_draw_pixel(c, x0 - y, y0 - x, color);
    }
}

void gfx_canvas_fill_circle(gfx_canvas_t *c, int16_t x0, int16_t y0, int16_t r, uint16_t color)
{
    gfx_canvas_draw_line(c, x0, y0 - r, x0, y0 + r, color);
    int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x;
        gfx_canvas_draw_line(c, x0 + x, y0 - y, x0 + x, y0 + y, color);
        gfx_canvas_draw_line(c, x0 - x, y0 - y, x0 - x, y0 + y, color);
        gfx_canvas_draw_line(c, x0 + y, y0 - x, x0 + y, y0 + x, color);
        gfx_canvas_draw_line(c, x0 - y, y0 - x, x0 - y, y0 + x, color);
    }
}

// ============================================================================
// ИСПРАВЛЕННАЯ ФУНКЦИЯ draw_bitmap_mono
// ============================================================================

void gfx_canvas_draw_bitmap_mono(gfx_canvas_t *c, int16_t x, int16_t y,
                                  const uint8_t *bitmap, int16_t w, int16_t h, uint16_t color)
{
    // Обрезаем по границам canvas
    int16_t start_x = (x < 0) ? -x : 0;
    int16_t start_y = (y < 0) ? -y : 0;
    int16_t end_x = (x + w > c->width) ? c->width - x : w;
    int16_t end_y = (y + h > c->height) ? c->height - y : h;
    
    if (start_x >= end_x || start_y >= end_y) return;
    
    int16_t bytes_per_row = (w + 7) / 8;
    
    // Рисуем только видимую часть (прямая запись в буфер)
    for (int16_t yy = start_y; yy < end_y; yy++) {
        uint16_t *row = &c->buf[(int32_t)(y + yy) * c->width + x];
        for (int16_t xx = start_x; xx < end_x; xx++) {
            uint8_t byte = bitmap[yy * bytes_per_row + (xx / 8)];
            if (byte & (0x80 >> (xx & 7))) {
                row[xx] = color;
            }
        }
    }
    
    // Отмечаем dirty rect один раз (а не на каждый пиксель!)
    mark_dirty(c, x + start_x, y + start_y, x + end_x, y + end_y);
}

void gfx_canvas_draw_bitmap_rgb565(gfx_canvas_t *c, int16_t x, int16_t y,
                                    const uint16_t *bitmap, int16_t w, int16_t h)
{
    for (int16_t yy = 0; yy < h; yy++) {
        for (int16_t xx = 0; xx < w; xx++) {
            gfx_canvas_draw_pixel(c, x + xx, y + yy, bitmap[yy * w + xx]);
        }
    }
}

// ---- text ----

void gfx_canvas_set_font(gfx_canvas_t *c, const gfx_font_t *font) { c->font = font; }
void gfx_canvas_set_cursor(gfx_canvas_t *c, int16_t x, int16_t y) { c->cursor_x = x; c->cursor_y = y; }

void gfx_canvas_set_text_color(gfx_canvas_t *c, uint16_t fg)
{
    c->text_color = fg;
    c->text_bg_opaque = false;
}

void gfx_canvas_set_text_color_bg(gfx_canvas_t *c, uint16_t fg, uint16_t bg)
{
    c->text_color = fg;
    c->text_bg_color = bg;
    c->text_bg_opaque = true;
}

static void draw_char(gfx_canvas_t *c, char ch)
{
    const gfx_font_t *f = c->font;
    if (!f || (uint8_t)ch < f->first || (uint8_t)ch > f->last) return;

    const gfx_glyph_t *g = &f->glyphs[(uint8_t)ch - f->first];
    const uint8_t *bmp = f->bitmap;

    if (c->text_bg_opaque) {
        gfx_canvas_fill_rect(c, c->cursor_x, c->cursor_y + g->yOffset,
                              g->xAdvance, f->yAdvance, c->text_bg_color);
    }

    uint16_t bo = g->bitmapOffset;
    uint8_t bits = 0, bit = 0;
    for (int16_t yy = 0; yy < g->height; yy++) {
        for (int16_t xx = 0; xx < g->width; xx++) {
            if (!(bit++ & 7)) {
                bits = bmp[bo++];
            }
            if (bits & 0x80) {
                gfx_canvas_draw_pixel(c, c->cursor_x + g->xOffset + xx,
                                      c->cursor_y + g->yOffset + yy, c->text_color);
            }
            bits <<= 1;
        }
    }
    c->cursor_x += g->xAdvance;
}

void gfx_canvas_print(gfx_canvas_t *c, const char *str)
{
    if (!c->font) return;
    while (*str) {
        if (*str == '\n') {
            c->cursor_x = 0;
            c->cursor_y += c->font->yAdvance;
        } else {
            draw_char(c, *str);
        }
        str++;
    }
}

void gfx_canvas_printf(gfx_canvas_t *c, const char *fmt, ...)
{
    char buf[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    gfx_canvas_print(c, buf);
}

// ---- flush ----

esp_err_t gfx_canvas_flush(gfx_canvas_t *c)
{
    if (c->dirty_x0 > c->dirty_x1) {
        return ESP_OK;
    }

    int16_t x0 = c->dirty_x0, y0 = c->dirty_y0, x1 = c->dirty_x1, y1 = c->dirty_y1;
    int16_t w = x1 - x0;

    esp_err_t err;
    if (x0 == 0 && x1 == c->width) {
        err = display_flush(x0, y0, x1, y1, &c->buf[(int32_t)y0 * c->width]);
    } else {
        uint16_t *scratch = heap_caps_malloc((size_t)w * (y1 - y0) * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!scratch) return ESP_ERR_NO_MEM;
        for (int16_t yy = y0; yy < y1; yy++) {
            memcpy(&scratch[(yy - y0) * w], &c->buf[(int32_t)yy * c->width + x0], w * sizeof(uint16_t));
        }
        err = display_flush(x0, y0, x1, y1, scratch);
        free(scratch);
    }

    clear_dirty(c);
    return err;
}
