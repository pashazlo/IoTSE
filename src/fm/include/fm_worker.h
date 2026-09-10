#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// FM Worker
//
// UI не выполняет файловые операции напрямую.
// UI отправляет команду в worker.
//
// Worker выполняет:
//   stat()
//   fopen()
//   fclose()
//   mkdir()
//   remove()
//   rename()
//   opendir()
//   readdir()
//   etc.
//
// После выполнения worker отправляет событие обратно.
// ============================================================================

#define FM_WORKER_NAME_LEN 64

typedef enum {
    FM_CMD_NONE = 0,

    FM_CMD_ENTER_VOLUME,
    FM_CMD_ENTER_DIR,
    FM_CMD_GO_UP,

    FM_CMD_CREATE_FILE,
    FM_CMD_CREATE_DIR,

    FM_CMD_DELETE,
    FM_CMD_RENAME,

    FM_CMD_OPEN_FILE,
    FM_CMD_SAVE_FILE,

} fm_worker_cmd_type_t;


typedef struct {
    fm_worker_cmd_type_t type;

    union {

        uint8_t volume_index;

        struct {
            char name[FM_WORKER_NAME_LEN];
        } single_name;

        struct {
            char old_name[FM_WORKER_NAME_LEN];
            char new_name[FM_WORKER_NAME_LEN];
        } rename;

        struct {
            char name[FM_WORKER_NAME_LEN];
            bool is_dir;
        } delete_entry;

        struct {
            char path[256];
        } file;

    } data;

} fm_worker_cmd_t;


// ============================================================================
// Events from worker -> UI
// ============================================================================

typedef enum {

    FM_EVT_NONE = 0,

    // Операция завершилась успешно.
    FM_EVT_OK,

    // Операция завершилась ошибкой.
    FM_EVT_ERROR,

    // Кэш текущей директории обновлён.
    FM_EVT_CACHE_UPDATED,

    // Файл успешно загружен.
    FM_EVT_FILE_LOADED,

    // Файл успешно сохранён.
    FM_EVT_FILE_SAVED,

} fm_worker_event_type_t;


typedef struct {

    fm_worker_event_type_t type;

    esp_err_t error;

} fm_worker_event_t;


// ============================================================================
// Initialization
// ============================================================================

esp_err_t fm_worker_init(void);


// ============================================================================
// UI -> Worker
//
// Эти функции НЕ ждут выполнения операции.
//
// Они только кладут команду в очередь.
//
// ESP_OK       -> команда принята
// ESP_ERR_FULL -> очередь занята
// ============================================================================

esp_err_t fm_worker_send_enter_volume(uint8_t volume_index);

esp_err_t fm_worker_send_enter_dir(const char *name);

esp_err_t fm_worker_send_go_up(void);

esp_err_t fm_worker_send_create_file(const char *name);

esp_err_t fm_worker_send_create_dir(const char *name);

esp_err_t fm_worker_send_delete(
    const char *name,
    bool is_dir
);

esp_err_t fm_worker_send_rename(
    const char *old_name,
    const char *new_name
);

esp_err_t fm_worker_send_open_file(const char *path);

esp_err_t fm_worker_send_save_file(const char *path);


// ============================================================================
// Worker -> UI
//
// Получение события НЕ должно блокировать UI.
// ============================================================================

bool fm_worker_receive_event(fm_worker_event_t *event);

#ifdef __cplusplus
}
#endif
