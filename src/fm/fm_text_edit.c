#include "fm_text_edit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "FM_TEXT_EDIT";

// Выделяем большой массив статически во избежание Stack Overflow
static char (*s_lines)[FM_EDIT_MAX_LINE_LEN] = NULL;
static uint16_t s_line_count = 0;
static char s_filepath[256] = {0};

void fm_text_edit_init(void) {
    if (s_lines == NULL) {
        s_lines = heap_caps_malloc(FM_EDIT_MAX_LINES * FM_EDIT_MAX_LINE_LEN, MALLOC_CAP_SPIRAM);
    }
    fm_text_edit_clear();
}

void fm_text_edit_clear(void) {
    memset(s_lines, 0, sizeof(s_lines));
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
    if (line_index >= s_line_count) {
        return "";
    }
    return s_lines[line_index];
}

uint16_t fm_text_edit_line_length(uint16_t line_index) {
    if (line_index >= s_line_count) {
        return 0;
    }
    return (uint16_t)strnlen(s_lines[line_index], FM_EDIT_MAX_LINE_LEN);
}

const char *fm_text_edit_get_filepath(void) {
    return s_filepath;
}

// ----------------------------------------------------------------------------
// БЕЗОПАСНАЯ ЗАГРУЗКА ФАЙЛА
// ----------------------------------------------------------------------------
bool fm_text_edit_open(const char *filepath) {
    if (!filepath || strlen(filepath) == 0) return false;

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

        // Обрезаем спецсимволы конца строки (\n и \r)
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[--len] = '\0';
        }

        strncpy(s_lines[idx], buffer, FM_EDIT_MAX_LINE_LEN - 1);
        s_lines[idx][FM_EDIT_MAX_LINE_LEN - 1] = '\0';
        idx++;

        // Вычитываем остаток длинной строки из файла
        if (!has_newline) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') {
                /* пропускаем хвост */
            }
        }
    }

    fclose(f);
    s_line_count = (idx > 0) ? idx : 1;
    ESP_LOGI(TAG, "Successfully loaded %u lines from %s", s_line_count, filepath);
    return true;
}

// ----------------------------------------------------------------------------
// БЕЗОПАСНОЕ СОХРАНЕНИЕ
// ----------------------------------------------------------------------------
bool fm_text_edit_save(void) {
    if (s_filepath[0] == '\0') {
        ESP_LOGE(TAG, "No filepath specified for save");
        return false;
    }
    return fm_text_edit_save_as(s_filepath);
}

bool fm_text_edit_save_as(const char *filepath) {
    if (!filepath || strlen(filepath) == 0) return false;

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
    if (line_index >= FM_EDIT_MAX_LINES) return false;

    while (s_line_count <= line_index) {
        s_lines[s_line_count][0] = '\0';
        s_line_count++;
    }
    return true;
}

// ----------------------------------------------------------------------------
// МОДИФИКАЦИЯ СТРОК И ИЗМЕНЕНИЕ ТЕКСТА
// ----------------------------------------------------------------------------
bool fm_text_edit_set_line(uint16_t line_index, const char *text) {
    if (line_index >= FM_EDIT_MAX_LINES) return false;
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
    if (s_line_count >= FM_EDIT_MAX_LINES) return false;

    uint16_t target_idx = line_index + 1;
    if (target_idx > s_line_count) {
        target_idx = s_line_count;
    }

    // Сдвигаем все строки ниже на одну позицию
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
    if (line >= FM_EDIT_MAX_LINES) return false;
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
    if (line >= s_line_count) return false;

    char *target_line = s_lines[line];
    size_t current_len = strnlen(target_line, FM_EDIT_MAX_LINE_LEN - 1);

    if (column >= current_len) return false;

    memmove(target_line + column, 
            target_line + column + 1, 
            current_len - column);

    return true;
}

bool fm_text_edit_backspace(uint16_t line, uint16_t column) {
    if (column == 0 || line >= s_line_count) return false;
    return fm_text_edit_delete_char(line, column - 1);
}
