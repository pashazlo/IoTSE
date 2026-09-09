#include "fm_text_edit.h"
#include "fm.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"


static const char *TAG = "FM_TEXT_EDIT";


// ============================================================================
// Editor state
// ============================================================================

// Массив строк документа:
//
// s_lines[0] -> первая строка
// s_lines[1] -> вторая строка
// ...
//
// Вся рабочая копия документа живёт в PSRAM.
// Во flash файл записывается только при fm_text_edit_save().

static char (*s_lines)[FM_EDIT_MAX_LINE_LEN] = NULL;

static uint16_t s_line_count = 0;

static char s_filepath[FM_MAX_PATH_LEN];


// ============================================================================
// Internal helpers
// ============================================================================

static bool editor_is_ready(void)
{
    return s_lines != NULL;
}


static void copy_line(char *dst, const char *src)
{
    if (dst == NULL) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    strncpy(dst, src, FM_EDIT_MAX_LINE_LEN - 1);

    dst[FM_EDIT_MAX_LINE_LEN - 1] = '\0';
}


// ============================================================================
// Init
// ============================================================================

void fm_text_edit_init(void)
{
    if (s_lines != NULL) {
        return;
    }

    size_t total_bytes =
        (size_t)FM_EDIT_MAX_LINES *
        (size_t)FM_EDIT_MAX_LINE_LEN;

    ESP_LOGI(
        TAG,
        "Allocating %u bytes for editor",
        (unsigned int)total_bytes
    );


    // Пробуем выделить рабочий буфер в PSRAM.
    s_lines = heap_caps_calloc(
        FM_EDIT_MAX_LINES,
        FM_EDIT_MAX_LINE_LEN,
        MALLOC_CAP_SPIRAM
    );


    if (s_lines == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to allocate editor buffer in PSRAM"
        );

        return;
    }


    s_line_count = 1;

    s_lines[0][0] = '\0';

    s_filepath[0] = '\0';


    ESP_LOGI(
        TAG,
        "Editor initialized successfully"
    );
}


// ============================================================================
// Clear
// ============================================================================

void fm_text_edit_clear(void)
{
    if (!editor_is_ready()) {

        fm_text_edit_init();

        if (!editor_is_ready()) {
            return;
        }
    }


    memset(
        s_lines,
        0,
        (size_t)FM_EDIT_MAX_LINES *
        FM_EDIT_MAX_LINE_LEN
    );


    s_line_count = 1;

    s_filepath[0] = '\0';
}


// ============================================================================
// Ensure line exists
// ============================================================================

bool fm_text_edit_ensure_line(uint16_t line_index)
{
    if (!editor_is_ready()) {

        fm_text_edit_init();

        if (!editor_is_ready()) {
            return false;
        }
    }


    if (line_index >= FM_EDIT_MAX_LINES) {
        return false;
    }


    while (s_line_count <= line_index) {

        s_lines[s_line_count][0] = '\0';

        s_line_count++;
    }


    return true;
}


// ============================================================================
// Getters
// ============================================================================

uint16_t fm_text_edit_line_count(void)
{
    if (!editor_is_ready()) {
        return 0;
    }

    return s_line_count;
}


const char *fm_text_edit_get_line(uint16_t line_index)
{
    if (!editor_is_ready()) {
        return "";
    }


    if (line_index >= s_line_count) {
        return "";
    }


    return s_lines[line_index];
}


uint16_t fm_text_edit_line_length(uint16_t line_index)
{
    if (!editor_is_ready()) {
        return 0;
    }


    if (line_index >= s_line_count) {
        return 0;
    }


    return (uint16_t)strnlen(
        s_lines[line_index],
        FM_EDIT_MAX_LINE_LEN
    );
}


const char *fm_text_edit_get_filepath(void)
{
    return s_filepath;
}


// ============================================================================
// Set line
// ============================================================================

bool fm_text_edit_set_line(
    uint16_t line_index,
    const char *text
)
{
    if (!fm_text_edit_ensure_line(line_index)) {
        return false;
    }


    copy_line(
        s_lines[line_index],
        text
    );


    return true;
}


// ============================================================================
// Insert line
// ============================================================================

bool fm_text_edit_insert_line_after(uint16_t line_index)
{
    if (!editor_is_ready()) {
        return false;
    }


    if (s_line_count >= FM_EDIT_MAX_LINES) {
        ESP_LOGW(TAG, "Maximum line count reached");
        return false;
    }


    if (line_index >= s_line_count) {
        return false;
    }


    // Сдвигаем строки вниз.
    //
    // Например:
    //
    // 0 AAA
    // 1 BBB
    // 2 CCC
    //
    // insert after 0:
    //
    // 0 AAA
    // 1 ""
    // 2 BBB
    // 3 CCC

    for (
        int32_t i = s_line_count;
        i > (int32_t)line_index + 1;
        i--
    ) {

        memcpy(
            s_lines[i],
            s_lines[i - 1],
            FM_EDIT_MAX_LINE_LEN
        );
    }


    s_lines[line_index + 1][0] = '\0';

    s_line_count++;


    return true;
}


// ============================================================================
// Delete line
// ============================================================================

bool fm_text_edit_delete_line(uint16_t line_index)
{
    if (!editor_is_ready()) {
        return false;
    }


    if (line_index >= s_line_count) {
        return false;
    }


    // Не даём документу стать документом из 0 строк.
    if (s_line_count == 1) {

        s_lines[0][0] = '\0';

        return true;
    }


    for (
        uint16_t i = line_index;
        i < s_line_count - 1;
        i++
    ) {

        memcpy(
            s_lines[i],
            s_lines[i + 1],
            FM_EDIT_MAX_LINE_LEN
        );
    }


    s_line_count--;

    s_lines[s_line_count][0] = '\0';


    return true;
}


// ============================================================================
// Insert text into line
// ============================================================================

bool fm_text_edit_insert_text(
    uint16_t line,
    uint16_t column,
    const char *text
)
{
    if (!editor_is_ready() || text == NULL) {
        return false;
    }


    if (line >= s_line_count) {
        return false;
    }


    char *target = s_lines[line];

    size_t current_len = strnlen(
        target,
        FM_EDIT_MAX_LINE_LEN
    );


    if (column > current_len) {
        column = current_len;
    }


    size_t insert_len = strlen(text);


    if (insert_len == 0) {
        return true;
    }


    // Сколько реально можем вставить.
    size_t available =
        (FM_EDIT_MAX_LINE_LEN - 1) - current_len;


    if (available == 0) {
        return false;
    }


    if (insert_len > available) {
        insert_len = available;
    }


    // Сдвигаем хвост строки вправо.
    memmove(
        target + column + insert_len,
        target + column,
        current_len - column + 1
    );


    memcpy(
        target + column,
        text,
        insert_len
    );


    return true;
}


// ============================================================================
// Delete character
// ============================================================================

bool fm_text_edit_delete_char(
    uint16_t line,
    uint16_t column
)
{
    if (!editor_is_ready()) {
        return false;
    }


    if (line >= s_line_count) {
        return false;
    }


    char *target = s_lines[line];

    size_t len = strnlen(
        target,
        FM_EDIT_MAX_LINE_LEN
    );


    if (column >= len) {
        return false;
    }


    memmove(
        target + column,
        target + column + 1,
        len - column
    );


    return true;
}


// ============================================================================
// Backspace
// ============================================================================

bool fm_text_edit_backspace(
    uint16_t line,
    uint16_t column
)
{
    if (!editor_is_ready()) {
        return false;
    }


    if (line >= s_line_count) {
        return false;
    }


    if (column == 0) {
        return false;
    }


    return fm_text_edit_delete_char(
        line,
        column - 1
    );
}


// ============================================================================
// Open file
// ============================================================================

bool fm_text_edit_open(const char *filepath)
{
    if (filepath == NULL) {
        return false;
    }


    if (!editor_is_ready()) {

        fm_text_edit_init();

        if (!editor_is_ready()) {
            return false;
        }
    }


    // Очищаем старый документ.
    memset(
        s_lines,
        0,
        (size_t)FM_EDIT_MAX_LINES *
        FM_EDIT_MAX_LINE_LEN
    );


    s_line_count = 1;


    FILE *file = fopen(filepath, "r");

    if (file == NULL) {

        // Если файла нет — создаём пустой документ.
        ESP_LOGW(
            TAG,
            "File not found, opening empty document: %s",
            filepath
        );


        copy_line(
            s_filepath,
            filepath
        );


        return true;
    }


    char buffer[FM_EDIT_MAX_LINE_LEN];


    uint16_t line = 0;


    while (
        fgets(buffer, sizeof(buffer), file) != NULL &&
        line < FM_EDIT_MAX_LINES
    ) {

        // Убираем \n и \r.
        buffer[
            strcspn(
                buffer,
                "\r\n"
            )
        ] = '\0';


        copy_line(
            s_lines[line],
            buffer
        );


        line++;
    }


    fclose(file);


    if (line == 0) {

        s_line_count = 1;

        s_lines[0][0] = '\0';

    } else {

        s_line_count = line;
    }


    copy_line(
        s_filepath,
        filepath
    );


    ESP_LOGI(
        TAG,
        "Opened file: %s (%u lines)",
        filepath,
        (unsigned int)s_line_count
    );


    return true;
}


// ============================================================================
// Save file
// ============================================================================

bool fm_text_edit_save(void)
{
    if (!editor_is_ready()) {
        return false;
    }


    if (s_filepath[0] == '\0') {
        ESP_LOGE(TAG, "No filepath set");
        return false;
    }


    return fm_text_edit_save_as(
        s_filepath
    );
}


// ============================================================================
// Save as
// ============================================================================

bool fm_text_edit_save_as(const char *filepath)
{
    if (!editor_is_ready() || filepath == NULL) {
        return false;
    }


    FILE *file = fopen(
        filepath,
        "w"
    );


    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to open for writing: %s",
            filepath
        );

        return false;
    }


    for (
        uint16_t i = 0;
        i < s_line_count;
        i++
    ) {

        fputs(
            s_lines[i],
            file
        );


        // Добавляем newline после каждой строки,
        // кроме последней.
        if (i < s_line_count - 1) {

            fputc(
                '\n',
                file
            );
        }
    }


    fclose(file);


    copy_line(
        s_filepath,
        filepath
    );


    ESP_LOGI(
        TAG,
        "Saved file: %s",
        filepath
    );


    return true;
}


// ============================================================================
// Close
// ============================================================================

void fm_text_edit_close(void)
{
    // Пока не освобождаем PSRAM.
    //
    // Буфер остаётся выделенным на всё время работы устройства.
    // Это нормально: всего 6.4 KB.
    //
    // Просто сбрасываем состояние документа.

    if (!editor_is_ready()) {
        return;
    }


    s_line_count = 1;

    s_lines[0][0] = '\0';

    s_filepath[0] = '\0';


    ESP_LOGI(TAG, "Editor closed");
}
