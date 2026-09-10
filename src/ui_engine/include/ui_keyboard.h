#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ui_event.h"
#include "gfx_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 63 bytes of ASCII text plus NUL, matching FM names and editor lines. */
#define UI_KEYBOARD_MAX_TEXT_LEN 64

/* Open a modal keyboard; initial text is copied and bounded. */
void ui_keyboard_open(const char *initial_text);
/* Cancel without applying the text. */
void ui_keyboard_cancel(void);
bool ui_keyboard_is_open(void);
bool ui_keyboard_was_confirmed(void);
/* Pointer remains owned by the keyboard. */
const char *ui_keyboard_get_text(void);
/* Arrows navigate; SELECT activates a character or OK/ESC/CLEAR. */
void ui_keyboard_handle_event(ui_event_t evt);
/* Draw on the existing 320x170 canvas, using the project's font. */
void ui_keyboard_draw(gfx_canvas_t *canvas);

#ifdef __cplusplus
}
#endif
