#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// RGB565 helper — build a color from 8-bit components, same convention
// Lopaka/Adafruit code uses when it hands you separate R,G,B.
#define GFX_RGB565(r,g,b)  ((((r)&0xF8)<<8) | (((g)&0xFC)<<3) | ((b)>>3))

#define GFX_RED     0xF800
#define GFX_GREEN   0x07E0
#define GFX_PINK    0xD809
#define GFX_BLACK   0x0000
#define GFX_WHITE   0xFFFF
#define GFX_BLUE      0x001F
#define GFX_YELLOW    0xFFE0
#define GFX_CYAN      0x07FF
#define GFX_MAGENTA   0xF81F
#define GFX_ORANGE    0xFD20
#define GFX_DARKGRAY  0x7BEF
#define GFX_LIGHTGRAY 0xC618
#define GFX_DARKGREEN 0x03E0

/**
 * @brief GFXfont-compatible font format (same layout Adafruit GFX and
 *        Lopaka's "Adafruit GFX" font export use), so a font exported
 *        from Lopaka can be dropped in as-is.
 */
typedef struct {
    uint16_t bitmapOffset;       // pointer into gfx_font_t.bitmap for this glyph's data
    uint8_t width, height;       // bitmap dimensions in bits (width) and rows
    uint8_t xAdvance;            // distance to move cursor for next char
    int8_t  xOffset, yOffset;    // dist from cursor pos to UL corner of bitmap
} gfx_glyph_t;

typedef struct {
    const uint8_t   *bitmap;    // packed 1-bit glyph bitmaps, MSB first
    const gfx_glyph_t *glyphs;  // one entry per codepoint in [first, last]
    uint8_t first, last;        // ASCII range this font covers
    uint8_t yAdvance;           // line height
} gfx_font_t;

/**
 * @brief An in-RAM RGB565 canvas. Every draw call writes into `buf`
 *        (fast, no SPI traffic) and widens the dirty rectangle. Nothing
 *        reaches the physical panel until gfx_canvas_flush().
 */
typedef struct {
    uint16_t *buf;
    int16_t width, height;

    // dirty rectangle, in canvas coordinates; x1/y1 are exclusive.
    // Starts "empty" (x0 > x1) after init/flush.
    int16_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;

    // text cursor/state, mirrors Adafruit_GFX's setCursor/setTextColor/print
    int16_t cursor_x, cursor_y;
    uint16_t text_color, text_bg_color;
    bool text_bg_opaque;        // false: transparent background (Adafruit default-ish)
    const gfx_font_t *font;     // NULL = no text support until you set one
} gfx_canvas_t;

/**
 * @brief Allocate a canvas of the given size. Uses DMA-capable memory
 *        so the buffer can be handed straight to display_flush().
 *        A full 240x320 canvas costs 240*320*2 = 150KB — fine on
 *        PSRAM-equipped boards, tight on internal SRAM alone. For
 *        SRAM-only boards, size canvases to just the region you're
 *        redrawing (e.g. a 240x40 status bar) instead of the full screen.
 */
esp_err_t gfx_canvas_init(gfx_canvas_t *canvas, int16_t width, int16_t height);
void gfx_canvas_deinit(gfx_canvas_t *canvas);

// --- Primitives (all clip to canvas bounds, all widen the dirty rect) ---
void gfx_canvas_fill(gfx_canvas_t *c, uint16_t color);                       // whole canvas
void gfx_canvas_draw_pixel(gfx_canvas_t *c, int16_t x, int16_t y, uint16_t color);
void gfx_canvas_fill_rect(gfx_canvas_t *c, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void gfx_canvas_draw_rect(gfx_canvas_t *c, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color); // outline
void gfx_canvas_draw_line(gfx_canvas_t *c, int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
void gfx_canvas_draw_circle(gfx_canvas_t *c, int16_t x0, int16_t y0, int16_t r, uint16_t color);
void gfx_canvas_fill_circle(gfx_canvas_t *c, int16_t x0, int16_t y0, int16_t r, uint16_t color);

// 1-bit bitmap (MSB-first packed rows), Adafruit drawBitmap-compatible:
// `color` is used for set bits, background stays untouched (transparent).
// This is what Lopaka exports monochrome icons as.
void gfx_canvas_draw_bitmap_mono(gfx_canvas_t *c, int16_t x, int16_t y,
                                  const uint8_t *bitmap, int16_t w, int16_t h, uint16_t color);

// Full-color RGB565 bitmap, opaque (used for the boot splash / photo-like art).
void gfx_canvas_draw_bitmap_rgb565(gfx_canvas_t *c, int16_t x, int16_t y,
                                    const uint16_t *bitmap, int16_t w, int16_t h);

// --- Text (Adafruit_GFX-style cursor/print API) ---
void gfx_canvas_set_font(gfx_canvas_t *c, const gfx_font_t *font);
void gfx_canvas_set_cursor(gfx_canvas_t *c, int16_t x, int16_t y);
void gfx_canvas_set_text_color(gfx_canvas_t *c, uint16_t fg);                      // transparent bg
void gfx_canvas_set_text_color_bg(gfx_canvas_t *c, uint16_t fg, uint16_t bg);      // opaque bg
void gfx_canvas_print(gfx_canvas_t *c, const char *str);
void gfx_canvas_printf(gfx_canvas_t *c, const char *fmt, ...);

/**
 * @brief Push exactly the dirty rectangle to the physical panel via
 *        display_flush(), then clears the dirty rect. If nothing was
 *        drawn since the last flush, this is a no-op (no SPI traffic).
 */
esp_err_t gfx_canvas_flush(gfx_canvas_t *c);

#ifdef __cplusplus
}
#endif
