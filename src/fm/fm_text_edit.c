#include "fm_text_edit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "FM_TEXT_EDIT";

static char s_lines[FM_EDIT_MAX_LINES][FM_EDIT_MAX_LINE_LEN];
static uint16_t s_line_count = 0;
static char s_filepath[256] = {0};

void fm_text_edit_init(void) {
    fm_text_edit_clear();
}

void fm_text_edit_clear(void) {
    memset(s_lines, 0, sizeof(s_lines));
    s_line_count = 1; // Всегда есть хотя бы одна пустая строка
    s_filepath[0] = '\0';
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
    return (uint16_t)strlen(s_lines[line_index]);
}

const char *fm_text_edit_get_filepath(void) {
    return s_filepath;
}

bool fm_text_edit_open(const char *filepath) {
    if (!filepath) return false;

    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        return false;
    }

    fm_text_edit_clear();
    strncpy(s_filepath, filepath, sizeof(s_filepath) - 1);

    char buffer[FM_EDIT_MAX_LINE_LEN];
    uint16_t idx = 0;

    while (fgets(buffer, sizeof(buffer), f) && idx < FM_EDIT_MAX_LINES) {
        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[--len] = '\0';
        }
        strncpy(s_lines[idx], buffer, FM_EDIT_MAX_LINE_LEN - 1);
        s_lines[idx][FM_EDIT_MAX_LINE_LEN - 1] = '\0';
        idx++;
    }

    fclose(f);
    s_line_count = (idx > 0) ? idx : 1;
    ESP_LOGI(TAG, "Loaded %u lines from %s", s_line_count, filepath);
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
    if (!filepath) return false;

    FILE *f = fopen(filepath, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        return false;
    }

    for (uint16_t i = 0; i < s_line_count; i++) {
        fprintf(f, "%s\n", s_lines[i]);
    }

    fclose(f);
    strncpy(s_filepath, filepath, sizeof(s_filepath) - 1);
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

bool fm_text_edit_insert_line_after(uint16_t line_index) {
    if (s_line_count >= FM_EDIT_MAX_LINES) return false;

    uint16_t target = line_index + 1;
    if (target < s_line_count) {
        memmove(&s_lines[target + 1], &s_lines[target], 
                (s_line_count - target) * FM_EDIT_MAX_LINE_LEN);
    }
    
    s_lines[target][0] = '\0';
    s_line_count++;
    return true;
}

bool fm_text_edit_delete_line(uint16_t line_index) {
    if (line_index >= s_line_count || s_line_count <= 1) return false;

    if (line_index < s_line_count - 1) {
        memmove(&s_lines[line_index], &s_lines[line_index + 1], 
                (s_line_count - line_index - 1) * FM_EDIT_MAX_LINE_LEN);
    }

    s_line_count--;
    s_lines[s_line_count][0] = '\0';
    return true;
}

bool fm_text_edit_insert_text(uint16_t line, uint16_t column, const char *text) {
    if (!text || text[0] == '\0') return true;
    if (!fm_text_edit_ensure_line(line)) return false;

    char *target_line = s_lines[line];
    size_t current_len = strlen(target_line);
    size_t insert_len = strlen(text);

    // Дополняем пробелами, если курсор ушел правее конца строки
    if (column > current_len) {
        if (column >= FM_EDIT_MAX_LINE_LEN - 1) return false;
        memset(target_line + current_len, ' ', column - current_len);
        target_line[column] = '\0';
        current_len = column;
    }

    if (current_len + insert_len >= FM_EDIT_MAX_LINE_LEN) {
        return false; 
    }

    // Сдвигаем и вставляем
    memmove(target_line + column + insert_len, 
            target_line + column, 
            current_len - column + 1);

    memcpy(target_line + column, text, insert_len);
    return true;
}

bool fm_text_edit_delete_char(uint16_t line, uint16_t column) {
    if (line >= s_line_count) return false;

    char *target_line = s_lines[line];
    size_t current_len = strlen(target_line);

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
