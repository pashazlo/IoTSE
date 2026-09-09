#include "fm_text_edit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "FM_TEXT_EDIT";

static char (*s_lines)[FM_EDIT_MAX_LINE_LEN] = NULL;
static uint16_t s_line_count = 0;
static char s_filepath[256] = {0};

void fm_text_edit_init(void) {
    if (s_lines != NULL) {
        return; // Уже инициализировано
    }

    size_t total_bytes = (size_t)FM_EDIT_MAX_LINES * FM_EDIT_MAX_LINE_LEN;
    ESP_LOGI(TAG, "Trying to allocate %u KB in PSRAM...", (unsigned int)(total_bytes / 1024));

    // Выделяем память в PSRAM
    s_lines = heap_caps_calloc(FM_EDIT_MAX_LINES, FM_EDIT_MAX_LINE_LEN, MALLOC_CAP_SPIRAM);

    if (s_lines == NULL) {
        ESP_LOGE(TAG, "CRITICAL ERROR: Failed to allocate PSRAM for text editor!");
        return;
    }

    ESP_LOGI(TAG, "✓ Editor PSRAM allocated successfully at %p", s_lines);
    s_line_count = 1;
    s_lines[0][0] = '\0';
    s_filepath[0] = '\0';
}

void fm_text_edit_clear(void) {
    if (s_lines == NULL) {
        fm_text_edit_init();
        if (s_lines == NULL) return;
    }

    s_lines[0][0] = '\0';
    s_line_count = 1;
    s_filepath[0] = '\0';
}

bool fm_text_edit_ensure_line(uint16_t line_index) {
    if (s_lines == NULL) {
        fm_text_edit_init();
        if (s_lines == NULL) return false;
    }

    if (line_index >= FM_EDIT_MAX_LINES) return false;

    while (s_line_count <= line_index) {
        s_lines[s_line_count][0] = '\0';
        s_line_count++;
    }
    return true;
}

bool fm_text_edit_set_line(uint16_t line_index, const char *text) {
    if (!fm_text_edit_ensure_line(line_index)) return false;

    if (!text) {
        s_lines[line_index][0] = '\0';
    } else {
        strncpy(s_lines[line_index], text, FM_EDIT_MAX_LINE_LEN - 1);
        s_lines[line_index][FM_EDIT_MAX_LINE_LEN - 1] = '\0';
    }
    return true;
}

uint16_t fm_text_edit_line_count(void) {
    return s_line_count;
}

const char *fm_text_edit_get_line(uint16_t line_index) {
    if (s_lines == NULL || line_index >= s_line_count) {
        return "";
    }
    return s_lines[line_index];
}

uint16_t fm_text_edit_line_length(uint16_t line_index) {
    if (s_lines == NULL || line_index >= s_line_count) {
        return 0;
    }
    return (uint16_t)strnlen(s_lines[line_index], FM_EDIT_MAX_LINE_LEN);
}
