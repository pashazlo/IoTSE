#ifndef UI_FILE_EDITOR_H
#define UI_FILE_EDITOR_H

#include <stdbool.h>

#include "ui_event.h"
#include "gfx_canvas.h"


void ui_file_editor_init(void);

bool ui_file_editor_open_file(
    const char *filepath
);

void ui_file_editor_handle_event(
    ui_event_t evt
);

void ui_file_editor_draw(
    gfx_canvas_t *canvas
);

#endif // UI_FILE_EDITOR_H
