#ifndef UI_FILE_EDITOR_H
#define UI_FILE_EDITOR_H

#include <stdbool.h>

#include "ui_event.h"
#include "ui_popup.h"
#include "fm_worker.h"
#include "gfx_canvas.h"


void ui_file_editor_init(void);

void ui_file_editor_open_loaded(void);

bool ui_file_editor_handle_event(
    ui_event_t evt
);

/* Apply the result after the shared modal keyboard closes. */
void ui_file_editor_handle_keyboard_result(void);

/* The caret blinks even when no button event is received. */
bool ui_file_editor_needs_tick(void);

void ui_file_editor_open_actions(void);

/* Returns true only after a successful Save & Exit. */
bool ui_file_editor_handle_popup_result(ui_popup_result_t result);

bool ui_file_editor_handle_worker_event(const fm_worker_event_t *event);

void ui_file_editor_draw(
    gfx_canvas_t *canvas
);

#endif // UI_FILE_EDITOR_H
