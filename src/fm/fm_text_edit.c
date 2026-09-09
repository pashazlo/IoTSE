#include "fm_text_edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"


// ============================================================================
// Logging
// ============================================================================

static const char *TAG = "FM_TEXT_EDIT";


// ============================================================================
// Editor Storage
// ============================================================================

// Двумерный массив:
//
// line 0 -> [64 chars]
// line 1 -> [64 chars]
// line 2 -> [64 chars]
//
// Указатель имеет тип:
// char (*)[FM_EDIT_MAX_LINE_LEN]
//
// Это позволяет обращаться:
//
// s_lines[line][column]
//
static char (*s_lines)[FM_EDIT_MAX_LINE_LEN] = NULL;


// Текущее количество существующих строк.
static uint16_t s_line_count = 0;


// Путь к открытому файлу.
static char s_filepath[FM_EDIT_MAX_PATH_LEN];


// ============================================================================
// Internal Helpers
// ============================================================================

static bool ensure_memory(void)
{
    if (s_lines != NULL) {
        return true;
    }

    size_t total_bytes =
        (size_t)FM_EDIT_MAX_LINES *
        FM_EDIT_MAX_LINE_LEN;

    ESP_LOGI(
        TAG,
        "Allocating %u bytes for editor",
        (unsigned int)total_bytes
    );


    // Сначала пробуем PSRAM.
    s_lines = heap_caps_calloc(
        FM_EDIT_MAX_LINES,
        FM_EDIT_MAX_LINE_LEN,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );


    // Если PSRAM недоступна — fallback во внутреннюю RAM.
    if (s_lines == NULL) {

        ESP_LOGW(
            TAG,
            "PSRAM allocation failed, using internal RAM"
        );

        s_lines = calloc(
            FM_EDIT_MAX_LINES,
            FM_EDIT_MAX_LINE_LEN
        );
    }


    if (s_lines == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to allocate editor memory"
        );

        return false;
    }


    ESP_LOGI(
        TAG,
        "Editor memory allocated: %p (%u bytes)",
        s_lines,
        (unsigned int)total_bytes
    );


    return true;
}


static void clear_document(void)
{
    if (s_lines == NULL) {
        return;
    }

    memset(
        s_lines,
        0,
        (size_t)FM_EDIT_MAX_LINES *
        FM_EDIT_MAX_LINE_LEN
    );

    s_line_count = 1;

    s_lines[0][0] = '\0';
}


// ============================================================================
// Initialization
// ============================================================================

void fm_text_edit_init(void)
{
    if (!ensure_memory()) {
        return;
    }

    if (s_line_count == 0) {
        clear_document();
    }

    s_filepath[0] = '\0';
}


void fm_text_edit_clear(void)
{
    if (!ensure_memory()) {
        return;
    }

    clear_document();

    s_filepath[0] = '\0';
}


void fm_text_edit_close(void)
{
    // Мы НЕ освобождаем PSRAM.
    //
    // Буфер редактора остаётся выделенным до перезагрузки.
    // Это безопаснее и избавляет от фрагментации памяти.

    if (s_lines == NULL) {
        return;
    }

    clear_document();

    s_filepath[0] = '\0';

    ESP_LOGI(TAG, "Editor closed");
}


// ============================================================================
// File Path
// ============================================================================

const char *fm_text_edit_get_filepath(void)
{
    return s_filepath;
}


// ============================================================================
// Document State
// ============================================================================

uint16_t fm_text_edit_line_count(void)
{
    return s_line_count;
}


const char *fm_text_edit_get_line(uint16_t line_index)
{
    if (
        s_lines == NULL ||
        line_index >= s_line_count
    ) {
        return "";
    }

    return s_lines[line_index];
}


uint16_t fm_text_edit_line_length(uint16_t line_index)
{
    if (
        s_lines == NULL ||
        line_index >= s_line_count
    ) {
        return 0;
    }

    return (uint16_t)strnlen(
        s_lines[line_index],
        FM_EDIT_MAX_LINE_LEN
    );
}


// ============================================================================
// Line Management
// ============================================================================

bool fm_text_edit_ensure_line(uint16_t line_index)
{
    if (!ensure_memory()) {
        return false;
    }

    if (line_index >= FM_EDIT_MAX_LINES) {
        return false;
    }


    while (
        s_line_count <= line_index &&
        s_line_count < FM_EDIT_MAX_LINES
    ) {

        s_lines[s_line_count][0] = '\0';

        s_line_count++;
    }


    return true;
}


bool fm_text_edit_set_line(
    uint16_t line_index,
    const char *text
)
{
    if (!fm_text_edit_ensure_line(line_index)) {
        return false;
    }


    if (text == NULL) {

        s_lines[line_index][0] = '\0';

        return true;
    }


    strncpy(
        s_lines[line_index],
        text,
        FM_EDIT_MAX_LINE_LEN - 1
    );

    s_lines[line_index][FM_EDIT_MAX_LINE_LEN - 1] = '\0';

    return true;
}


bool fm_text_edit_insert_line_after(uint16_t line_index)
{
    if (!ensure_memory()) {
        return false;
    }


    if (line_index >= s_line_count) {
        return false;
    }


    if (s_line_count >= FM_EDIT_MAX_LINES) {

        ESP_LOGW(TAG, "Maximum line count reached");

        return false;
    }


    // Сдвигаем строки вниз.
    //
    // Было:
    //
    // 0 AAA
    // 1 BBB
    // 2 CCC
    //
    // insert after 0:
    //
    // 0 AAA
    // 1 empty
    // 2 BBB
    // 3 CCC

    for (
        uint16_t i = s_line_count;
        i > line_index + 1;
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


bool fm_text_edit_delete_line(uint16_t line_index)
{
    if (
        s_lines == NULL ||
        line_index >= s_line_count
    ) {
        return false;
    }


    // Последнюю строку физически не удаляем.
    // Документ всегда содержит хотя бы одну строку.
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
// Text Editing
// ============================================================================

bool fm_text_edit_insert_text(
    uint16_t line,
    uint16_t column,
    const char *text
)
{
    if (text == NULL) {
        return false;
    }


    if (!fm_text_edit_ensure_line(line)) {
        return false;
    }


    char *target = s_lines[line];


    size_t line_len = strnlen(
        target,
        FM_EDIT_MAX_LINE_LEN
    );


    if (column > line_len) {
        column = (uint16_t)line_len;
    }


    size_t text_len = strlen(text);


    size_t max_insert =
        (FM_EDIT_MAX_LINE_LEN - 1) - line_len;


    if (text_len > max_insert) {
        text_len = max_insert;
    }


    if (text_len == 0) {
        return true;
    }


    // Сдвигаем существующий текст вправо.
    memmove(
        target + column + text_len,
        target + column,
        line_len - column + 1
    );


    memcpy(
        target + column,
        text,
        text_len
    );


    return true;
}


bool fm_text_edit_delete_char(
    uint16_t line,
    uint16_t column
)
{
    if (
        s_lines == NULL ||
        line >= s_line_count
    ) {
        return false;
    }


    char *target = s_lines[line];


    size_t line_len = strnlen(
        target,
        FM_EDIT_MAX_LINE_LEN
    );


    if (column >= line_len) {
        return false;
    }


    memmove(
        target + column,
        target + column + 1,
        line_len - column
    );


    return true;
}


bool fm_text_edit_backspace(
    uint16_t line,
    uint16_t column
)
{
    if (
        s_lines == NULL ||
        line >= s_line_count
    ) {
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
// File Loading
// ============================================================================

bool fm_text_edit_open(const char *filepath)
{
    if (
        filepath == NULL ||
        filepath[0] == '\0'
    ) {
        return false;
    }


    if (!ensure_memory()) {
        return false;
    }


    ESP_LOGI(
        TAG,
        "Opening file: %s",
        filepath
    );


    FILE *file = fopen(filepath, "r");


    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to open file: %s",
            filepath
        );

        return false;
    }


    clear_document();


    uint16_t line_index = 0;


    char buffer[FM_EDIT_MAX_LINE_LEN];


    while (
        line_index < FM_EDIT_MAX_LINES &&
        fgets(
            buffer,
            sizeof(buffer),
            file
        ) != NULL
    ) {

        // Убираем \n и \r.
        buffer[
            strcspn(
                buffer,
                "\r\n"
            )
        ] = '\0';


        strncpy(
            s_lines[line_index],
            buffer,
            FM_EDIT_MAX_LINE_LEN - 1
        );

        s_lines[line_index]
        [FM_EDIT_MAX_LINE_LEN - 1] = '\0';


        line_index++;
    }


    fclose(file);


    // Даже пустой файл должен иметь одну строку.
    if (line_index == 0) {
        s_line_count = 1;
    } else {
        s_line_count = line_index;
    }


    strncpy(
        s_filepath,
        filepath,
        sizeof(s_filepath) - 1
    );

    s_filepath[
        sizeof(s_filepath) - 1
    ] = '\0';


    ESP_LOGI(
        TAG,
        "Loaded %u lines",
        (unsigned int)s_line_count
    );


    return true;
}


// ============================================================================
// File Saving
// ============================================================================

bool fm_text_edit_save(void)
{
    if (s_filepath[0] == '\0') {

        ESP_LOGE(
            TAG,
            "No file opened"
        );

        return false;
    }


    return fm_text_edit_save_as(
        s_filepath
    );
}


bool fm_text_edit_save_as(const char *filepath)
{
    if (
        filepath == NULL ||
        filepath[0] == '\0'
    ) {
        return false;
    }


    if (s_lines == NULL) {
        return false;
    }


    ESP_LOGI(
        TAG,
        "Saving file: %s",
        filepath
    );


    FILE *file = fopen(
        filepath,
        "w"
    );


    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to create file: %s",
            filepath
        );

        return false;
    }


    for (
        uint16_t i = 0;
        i < s_line_count;
        i++
    ) {

        if (
            fputs(
                s_lines[i],
                file
            ) == EOF
        ) {

            ESP_LOGE(
                TAG,
                "Write error on line %u",
                (unsigned int)i
            );

            fclose(file);

            return false;
        }


        // После последней строки перевод не обязателен.
        if (i < s_line_count - 1) {

            fputc(
                '\n',
                file
            );
        }
    }


    fclose(file);


    strncpy(
        s_filepath,
        filepath,
        sizeof(s_filepath) - 1
    );

    s_filepath[
        sizeof(s_filepath) - 1
    ] = '\0';


    ESP_LOGI(
        TAG,
        "File saved successfully"
    );


    return true;
}
