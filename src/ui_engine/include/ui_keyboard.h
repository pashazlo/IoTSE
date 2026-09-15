#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ui_event.h"
#include "gfx_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 511 bytes of text plus NUL, matching the editor line buffer. */
#define UI_KEYBOARD_MAX_TEXT_LEN 512

/* Open a modal keyboard; initial text is copied and bounded. */
void ui_keyboard_open(const char *initial_text);
/* Editor mode shows "NNN text|" and edits a temporary copy of one line. */
void ui_keyboard_open_editor(
    const char *line_text,
    uint16_t cursor_column,
    uint16_t line_number
);
/* Cancel without applying the text. */
void ui_keyboard_cancel(void);
bool ui_keyboard_is_open(void);
bool ui_keyboard_was_confirmed(void);
/* Pointer remains owned by the keyboard. */
const char *ui_keyboard_get_text(void);
uint16_t ui_keyboard_get_cursor_column(void);
/* Arrows navigate; SELECT activates a character or OK/ESC/CLEAR. */
void ui_keyboard_handle_event(ui_event_t evt);
/* Draw on the existing 320x170 canvas, using the project's font. */
void ui_keyboard_draw(gfx_canvas_t *canvas);

#ifdef __cplusplus
}
#endif
