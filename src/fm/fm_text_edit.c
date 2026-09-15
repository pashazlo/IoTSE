#include "fm_text_edit.h"
#include "fm.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"


static const char *TAG = "FM_TEXT_EDIT";

#define FM_EDIT_TEMP_SUFFIX   ".iotse.tmp"
#define FM_EDIT_BACKUP_SUFFIX ".iotse.bak"


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
static bool s_has_trailing_newline = false;


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
    s_has_trailing_newline = false;


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
    s_has_trailing_newline = false;

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

static bool make_companion_path(
    const char *filepath,
    const char *suffix,
    char *out,
    size_t out_size
)
{
    size_t path_len = strnlen(filepath, FM_MAX_PATH_LEN);
    size_t suffix_len = strlen(suffix);
    if (path_len == FM_MAX_PATH_LEN || path_len + suffix_len >= out_size) {
        return false;
    }
    memcpy(out, filepath, path_len);
    memcpy(out + path_len, suffix, suffix_len + 1);
    return true;
}

static bool preserve_artifact(
    const char *artifact_path,
    const char *filepath,
    const char *preserved_suffix
)
{
    char preserved_path[FM_MAX_PATH_LEN + 32];
    if (!make_companion_path(filepath, preserved_suffix,
                             preserved_path, sizeof(preserved_path))) {
        ESP_LOGE(TAG, "Cannot preserve recovery artifact: %s", artifact_path);
        return false;
    }
    struct stat info;
    if (stat(preserved_path, &info) == 0) {
        ESP_LOGE(TAG, "Preserved recovery name already exists: %s", preserved_path);
        return false;
    }
    if (rename(artifact_path, preserved_path) != 0) {
        ESP_LOGE(TAG, "Failed to preserve recovery artifact: %s", artifact_path);
        return false;
    }
    ESP_LOGW(TAG, "Preserved conflicting file as: %s", preserved_path);
    return true;
}

static bool reconcile_save_artifacts(const char *filepath)
{
    char temp_path[FM_MAX_PATH_LEN + sizeof(FM_EDIT_TEMP_SUFFIX)];
    char backup_path[FM_MAX_PATH_LEN + sizeof(FM_EDIT_BACKUP_SUFFIX)];
    if (!make_companion_path(filepath, FM_EDIT_TEMP_SUFFIX,
                             temp_path, sizeof(temp_path)) ||
        !make_companion_path(filepath, FM_EDIT_BACKUP_SUFFIX,
                             backup_path, sizeof(backup_path))) {
        ESP_LOGE(TAG, "Recovery path is too long: %s", filepath);
        return false;
    }

    struct stat info;
    bool target_exists = stat(filepath, &info) == 0;
    bool backup_exists = stat(backup_path, &info) == 0;
    bool temp_exists = stat(temp_path, &info) == 0;

    if (target_exists) {
        /* Existing suffixed files may be legitimate user data. Never delete
           them merely because their names resemble transaction artifacts. */
        if (temp_exists && !preserve_artifact(
                temp_path, filepath, ".recovered-tmp")) {
            return false;
        }
        if (backup_exists && !preserve_artifact(
                backup_path, filepath, ".recovered-bak")) {
            return false;
        }
        return true;
    }

    if (backup_exists) {
        /* Prefer the last known committed document over an uncommitted temp. */
        if (temp_exists && !preserve_artifact(
                temp_path, filepath, ".recovered-tmp")) {
            return false;
        }
        if (rename(backup_path, filepath) != 0) {
            ESP_LOGE(TAG, "Failed to restore backup: %s", backup_path);
            return false;
        }
        ESP_LOGW(TAG, "Recovered interrupted save: %s", filepath);
        return true;
    }

    if (temp_exists) {
        if (rename(temp_path, filepath) != 0) {
            ESP_LOGE(TAG, "Failed to recover temporary file: %s", temp_path);
            return false;
        }
        ESP_LOGW(TAG, "Recovered new file from temporary save: %s", filepath);
    }
    return true;
}


bool fm_text_edit_split_line(uint16_t line_index, uint16_t column)
{
    if (!editor_is_ready() || line_index >= s_line_count) {
        return false;
    }

    size_t line_len = strnlen(s_lines[line_index], FM_EDIT_MAX_LINE_LEN);
    if (column > line_len) {
        column = (uint16_t)line_len;
    }

    char tail[FM_EDIT_MAX_LINE_LEN];
    copy_line(tail, s_lines[line_index] + column);

    if (!fm_text_edit_insert_line_after(line_index)) {
        return false;
    }

    s_lines[line_index][column] = '\0';
    copy_line(s_lines[line_index + 1], tail);
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


    size_t insert_len = strlen(text);


    if (insert_len == 0) {
        return true;
    }


    // Сколько реально можем вставить.
    /* A vertical move keeps the preferred visual column.  When text is
       inserted on a shorter line, materialize the gap as spaces so the
       insertion really happens at the visible caret position. */
    if (column > current_len) {
        if (column >= FM_EDIT_MAX_LINE_LEN) {
            return false;
        }

        memset(target + current_len, ' ', column - current_len);
        target[column] = '\0';
        current_len = column;
    }

    size_t available = (FM_EDIT_MAX_LINE_LEN - 1) - current_len;


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


bool fm_text_edit_backspace_at(uint16_t *line, uint16_t *column)
{
    if (!editor_is_ready() || line == NULL || column == NULL) {
        return false;
    }

    if (*line >= s_line_count) {
        return false;
    }

    size_t current_len = fm_text_edit_line_length(*line);
    if (*column > current_len) {
        *column = (uint16_t)current_len;
    }

    if (*column > 0) {
        if (!fm_text_edit_backspace(*line, *column)) {
            return false;
        }
        (*column)--;
        return true;
    }

    if (*line == 0) {
        return false;
    }

    uint16_t previous_line = *line - 1;
    size_t previous_len = fm_text_edit_line_length(previous_line);
    if (previous_len + current_len >= FM_EDIT_MAX_LINE_LEN) {
        return false;
    }

    memcpy(
        s_lines[previous_line] + previous_len,
        s_lines[*line],
        current_len + 1
    );

    if (!fm_text_edit_delete_line(*line)) {
        return false;
    }

    *line = previous_line;
    *column = (uint16_t)previous_len;
    return true;
}


// ============================================================================
// Open file
// ============================================================================

bool fm_text_edit_open(const char *filepath)
{
    if (filepath == NULL) {
        return false;
    }

    if (!reconcile_save_artifacts(filepath)) {
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
    s_has_trailing_newline = false;


    FILE *file = fopen(filepath, "r");

    if (file == NULL) {

        // Ошибка открытия: не выдаём пустой документ за успешно загруженный.
        ESP_LOGW(
            TAG,
            "Failed to open file: %s",
            filepath
        );


        snprintf(s_filepath, sizeof(s_filepath), "%s", filepath);


        return false;
    }


    uint16_t line = 0;
    size_t col = 0;
    int ch;
    bool too_large = false;
    while ((ch = fgetc(file)) != EOF) {
        if (line >= FM_EDIT_MAX_LINES) { too_large = true; break; }
        if (ch == '\r') continue;
        if (ch == '\n') {
            s_lines[line][col] = '\0';
            line++;
            col = 0;
            s_has_trailing_newline = true;
        } else {
            if (col >= FM_EDIT_MAX_LINE_LEN - 1 || ch == 0) {
                too_large = true;
                break;
            }
            s_lines[line][col++] = (char)ch;
            s_has_trailing_newline = false;
        }
    }
    bool read_failed = ferror(file) != 0;
    if (fclose(file) != 0) read_failed = true;
    if (read_failed || too_large) {
        s_filepath[0] = '\0';
        ESP_LOGE(TAG, "Cannot edit file: read error or text exceeds editor limits");
        return false;
    }
    if (col > 0) line++;
    s_line_count = line ? line : 1;

    if (filepath != s_filepath) {
        snprintf(s_filepath, sizeof(s_filepath), "%s", filepath);
    }


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
    if (!editor_is_ready() || filepath == NULL || filepath[0] == '\0') {
        return false;
    }

    if (!reconcile_save_artifacts(filepath)) {
        return false;
    }

    char temp_path[FM_MAX_PATH_LEN + sizeof(FM_EDIT_TEMP_SUFFIX)];
    char backup_path[FM_MAX_PATH_LEN + sizeof(FM_EDIT_BACKUP_SUFFIX)];
    if (!make_companion_path(filepath, FM_EDIT_TEMP_SUFFIX,
                             temp_path, sizeof(temp_path)) ||
        !make_companion_path(filepath, FM_EDIT_BACKUP_SUFFIX,
                             backup_path, sizeof(backup_path))) {
        ESP_LOGE(TAG, "Save path is too long");
        return false;
    }

    (void)remove(temp_path);

    FILE *file = fopen(
        temp_path,
        "w"
    );


    if (file == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to open for writing: %s",
            temp_path
        );

        return false;
    }


    for (
        uint16_t i = 0;
        i < s_line_count;
        i++
    ) {

        if (fputs(s_lines[i], file) == EOF) {
            (void)fclose(file);
            (void)remove(temp_path);
            return false;
        }


        // Добавляем newline после каждой строки,
        // кроме последней.
        if (i < s_line_count - 1) {

            if (fputc('\n', file) == EOF) {
                (void)fclose(file);
                (void)remove(temp_path);
                return false;
            }
        }
    }

    if (s_has_trailing_newline && fputc('\n', file) == EOF) {
        (void)fclose(file);
        (void)remove(temp_path);
        return false;
    }

    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        ESP_LOGE(TAG, "Failed to flush temporary save: %s", temp_path);
        (void)fclose(file);
        (void)remove(temp_path);
        return false;
    }

    if (fclose(file) != 0) {
        (void)remove(temp_path);
        return false;
    }

    struct stat info;
    bool target_exists = stat(filepath, &info) == 0;
    bool backup_exists = stat(backup_path, &info) == 0;

    if (target_exists) {
        if (backup_exists) {
            ESP_LOGE(TAG, "Backup already exists, refusing save: %s", backup_path);
            (void)remove(temp_path);
            return false;
        }
        if (rename(filepath, backup_path) != 0) {
            ESP_LOGE(TAG, "Failed to create backup: %s", filepath);
            (void)remove(temp_path);
            return false;
        }
    }

    if (rename(temp_path, filepath) != 0) {
        ESP_LOGE(TAG, "Failed to commit saved file: %s", filepath);
        if (target_exists && rename(backup_path, filepath) != 0) {
            ESP_LOGE(TAG, "Failed to restore backup: %s", backup_path);
        }
        (void)remove(temp_path);
        return false;
    }

    if (target_exists && remove(backup_path) != 0) {
        ESP_LOGW(TAG, "Saved, but backup remains: %s", backup_path);
    }

    if (filepath != s_filepath) {
        snprintf(s_filepath, sizeof(s_filepath), "%s", filepath);
    }


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
    s_has_trailing_newline = false;


    ESP_LOGI(TAG, "Editor closed");
}

