#include "fm_text_edit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "FM_TEXT_EDIT";

// Указатель на массив строк в PSRAM
static char (*s_lines)[FM_EDIT_MAX_LINE_LEN] = NULL;
static uint16_t s_line_count = 0;
static char s_filepath[256] = {0};

void fm_text_edit_init(void) {
    if (s_lines == NULL) {
        size_t size = FM_EDIT_MAX_LINES * FM_EDIT_MAX_LINE_LEN;
        
        // Выделяем память в PSRAM (8 МБ)
        s_lines = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        
        if (s_lines == NULL) {
            ESP_LOGE(TAG, "CRITICAL: PSRAM allocation failed (%u bytes)!", (unsigned int)size);
            return;
        }
        ESP_LOGI(TAG, "Successfully allocated %u KB in PSRAM for editor", (unsigned int)(size / 1024));
    }
    fm_text_edit_clear();
}

void fm_text_edit_clear(void) {
    if (s_lines == NULL) return;

    memset(s_lines, 0, FM_EDIT_MAX_LINES * FM_EDIT_MAX_LINE_LEN);
    s_line_count = 1;
    s_filepath[0] = '\0';
}

void fm_text_edit_close(void) {
    fm_text_edit_clear();
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

const char *fm_text_edit_get_filepath(void) {
    return s_filepath;
}

bool fm_text_edit_open(const char *filepath) {
    if (!filepath || strlen(filepath) == 0) return false;
    if (s_lines == NULL) {
        fm_text_edit_init();
        if (s_lines == NULL) return false;
    }

    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return false;
    }

    fm_text_edit_clear();
    strncpy(s_filepath, filepath, sizeof(s_filepath) - 1);
    s_filepath[sizeof(s_filepath) - 1] = '\0';

    uint16_t idx = 0;
    char buffer[FM_EDIT_MAX_LINE_LEN];

    while (idx < FM_EDIT_MAX_LINES && fgets(buffer, sizeof(buffer), f) != NULL) {
        size_t len = strlen(buffer);
        
        bool has_newline = false;
        if (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            has_newline = true;
        }

        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[--len] = '\0';
        }

        strncpy(s_lines[idx], buffer, FM_EDIT_MAX_LINE_LEN - 1);
        s_lines[idx][FM_EDIT_MAX_LINE_LEN - 1] = '\0';
        idx++;

        if (!has_newline) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
                /* пропускаем длинный хвост */
            }
        }
    }

    fclose(f);
    s_line_count = (idx > 0) ? idx : 1;
    ESP_LOGI(TAG, "Loaded %u lines from %s into PSRAM", s_line_count, filepath);
    return true;
}

bool fm_text_edit_save(void) {
    if (s_filepath[0] == '\0') {
        ESP_LOGE(TAG, "No filepath specified for save");
        return false;
    }
    return fm_text_edit_save_as(s_filepath);
}

bool fm_text_edit_save_as(const char *filepath) {
    if (!filepath || strlen(filepath) == 0 || s_lines == NULL) return false;

    FILE *f = fopen(filepath, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        return false;
    }

    for (uint16_t i = 0; i < s_line_count; i++) {
        if (fputs(s_lines[i], f) == EOF || fputs("\n", f) == EOF) {
            ESP_LOGE(TAG, "Write error at line %u", i);
            fclose(f);
            return false;
        }
    }

    fclose(f);
    strncpy(s_filepath, filepath, sizeof(s_filepath) - 1);
    s_filepath[sizeof(s_filepath) - 1] = '\0';
    ESP_LOGI(TAG, "Saved %u lines to %s", s_line_count, filepath);
    return true;
}

bool fm_text_edit_ensure_line(uint16_t line_index) {
    if (s_lines == NULL || line_index >= FM_EDIT_MAX_LINES) return false;

    while (s_line_count <= line_index) {
        s_lines[s_line_count][0] = '\0';
        s_line_count++;
    }
    return true;
}

bool fm_text_edit_set_line(uint16_t line_index, const char *text) {
    if (s_lines == NULL || line_index >= FM_EDIT_MAX_LINES) return false;
    if (!fm_text_edit_ensure_line(line_index)) return false;

    if (!text) {
        s_lines[line_index][0] = '\0';
    } else {
        strncpy(s_lines[line_index], text, FM_EDIT_MAX_LINE_LEN - 1);
        s_lines[line_index][FM_EDIT_MAX_LINE_LEN - 1] = '\0';
    }
    return true;
}

bool fm_text_edit_insert_line_after(uint16_t line_index) {
    if (s_lines == NULL || s_line_count >= FM_EDIT_MAX_LINES) return false;

    uint16_t target_idx = line_index + 1;
    if (target_idx > s_line_count) {
        target_idx = s_line_count;
    }

    if (target_idx < s_line_count) {
        memmove(&s_lines[target_idx + 1],
                &s_lines[target_idx],
                (s_line_count - target_idx) * FM_EDIT_MAX_LINE_LEN);
    }

    s_lines[target_idx][0] = '\0';
    s_line_count++;
    return true;
}

bool fm_text_edit_insert_text(uint16_t line, uint16_t column, const char *text) {
    if (!text || text[0] == '\0') return true;
    if (s_lines == NULL || line >= FM_EDIT_MAX_LINES) return false;
    if (!fm_text_edit_ensure_line(line)) return false;

    char *target_line = s_lines[line];
    size_t current_len = strnlen(target_line, FM_EDIT_MAX_LINE_LEN - 1);
    size_t insert_len = strlen(text);

    if (column > current_len) {
        if (column >= FM_EDIT_MAX_LINE_LEN - 1) return false;
        memset(target_line + current_len, ' ', column - current_len);
        target_line[column] = '\0';
        current_len = column;
    }

    if (current_len + insert_len >= FM_EDIT_MAX_LINE_LEN - 1) {
        ESP_LOGW(TAG, "Line overflow prevented!");
        return false; 
    }

    size_t bytes_to_move = current_len - column + 1;
    memmove(target_line + column + insert_len, 
            target_line + column, 
            bytes_to_move);

    memcpy(target_line + column, text, insert_len);
    return true;
}

bool fm_text_edit_delete_char(uint16_t line, uint16_t column) {
    if (s_lines == NULL || line >= s_line_count) return false;

    char *target_line = s_lines[line];
    size_t current_len = strnlen(target_line, FM_EDIT_MAX_LINE_LEN - 1);

    if (column >= current_len) return false;

    memmove(target_line + column, 
            target_line + column + 1, 
            current_len - column);

    return true;
}

bool fm_text_edit_backspace(uint16_t line, uint16_t column) {
    if (column == 0 || s_lines == NULL || line >= s_line_count) return false;
    return fm_text_edit_delete_char(line, column - 1);
}
