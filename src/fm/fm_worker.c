#include "fm_worker.h"

#include <string.h>

#include "fm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"


static const char *TAG = "FM_WORKER";


// ============================================================================
// Configuration
// ============================================================================

#define FM_WORKER_QUEUE_LEN       8
#define FM_WORKER_EVENT_QUEUE_LEN 8

#define FM_WORKER_TASK_STACK      8192
#define FM_WORKER_TASK_PRIORITY   2


// ============================================================================
// Queues
// ============================================================================

static QueueHandle_t s_cmd_queue = NULL;
static QueueHandle_t s_event_queue = NULL;

static TaskHandle_t s_worker_task = NULL;


// ============================================================================
// Internal helpers
// ============================================================================

static void send_event(
    fm_worker_event_type_t type,
    esp_err_t error
)
{
    if (s_event_queue == NULL) {
        return;
    }

    fm_worker_event_t event = {
        .type = type,
        .error = error,
    };

    /*
     * ВАЖНО:
     *
     * Worker никогда не должен зависать из-за UI.
     *
     * Поэтому event queue тоже отправляем без ожидания.
     *
     * Если UI почему-то не успевает забирать события,
     * событие просто теряется, но worker продолжает работу.
     */
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, event dropped: %d", type);
    }
}


// ============================================================================
// Command execution
// ============================================================================

static void process_command(const fm_worker_cmd_t *cmd)
{
    if (cmd == NULL) {
        return;
    }

    esp_err_t err = ESP_OK;

    switch (cmd->type) {

        // ------------------------------------------------------------
        // Navigation
        // ------------------------------------------------------------

        case FM_CMD_ENTER_VOLUME:

            ESP_LOGI(
                TAG,
                "ENTER_VOLUME: %u",
                cmd->data.volume_index
            );

            err = fm_enter_volume(
                cmd->data.volume_index
            );

            break;


        case FM_CMD_ENTER_DIR:

            ESP_LOGI(
                TAG,
                "ENTER_DIR: %s",
                cmd->data.single_name.name
            );

            err = fm_enter_dir(
                cmd->data.single_name.name
            );

            break;


        case FM_CMD_GO_UP:

            ESP_LOGI(TAG, "GO_UP");

            if (!fm_go_up()) {
                err = ESP_ERR_INVALID_STATE;
            }

            break;


        // ------------------------------------------------------------
        // Create
        // ------------------------------------------------------------

        case FM_CMD_CREATE_FILE:

            ESP_LOGI(
                TAG,
                "CREATE_FILE: %s",
                cmd->data.single_name.name
            );

            err = fm_create_file(
                cmd->data.single_name.name
            );

            break;


        case FM_CMD_CREATE_DIR:

            ESP_LOGI(
                TAG,
                "CREATE_DIR: %s",
                cmd->data.single_name.name
            );

            err = fm_create_dir(
                cmd->data.single_name.name
            );

            break;


        // ------------------------------------------------------------
        // Delete
        // ------------------------------------------------------------

        case FM_CMD_DELETE:

            ESP_LOGI(
                TAG,
                "DELETE: %s (dir=%d)",
                cmd->data.delete_entry.name,
                cmd->data.delete_entry.is_dir
            );

            err = fm_delete_entry(
                cmd->data.delete_entry.name,
                cmd->data.delete_entry.is_dir
            );

            break;


        // ------------------------------------------------------------
        // Rename
        // ------------------------------------------------------------

        case FM_CMD_RENAME:

            ESP_LOGI(
                TAG,
                "RENAME: %s -> %s",
                cmd->data.rename.old_name,
                cmd->data.rename.new_name
            );

            err = fm_rename(
                cmd->data.rename.old_name,
                cmd->data.rename.new_name
            );

            break;


        // ------------------------------------------------------------
        // File open/save
        //
        // Пока это только задел.
        //
        // Реальный editor buffer мы подключим следующим этапом.
        // ------------------------------------------------------------

        case FM_CMD_OPEN_FILE:

            ESP_LOGI(
                TAG,
                "OPEN_FILE: %s",
                cmd->data.file.path
            );

            /*
             * Пока ничего не делаем.
             *
             * Загрузка текста будет вынесена в отдельный
             * editor/file worker path, чтобы не смешивать
             * browser и editor.
             */

            err = ESP_OK;

            break;


        case FM_CMD_SAVE_FILE:

            ESP_LOGI(
                TAG,
                "SAVE_FILE: %s",
                cmd->data.file.path
            );

            /*
             * Аналогично:
             * реальное сохранение буфера сделаем отдельно.
             */

            err = ESP_OK;

            break;


        default:

            ESP_LOGW(
                TAG,
                "Unknown command: %d",
                cmd->type
            );

            err = ESP_ERR_INVALID_ARG;

            break;
    }


    // ========================================================================
    // Result
    // ========================================================================

    if (err == ESP_OK) {

        send_event(
            FM_EVT_OK,
            ESP_OK
        );

        /*
         * Текущий fm.c сам обновляет cache после операций.
         *
         * Поэтому пока просто сообщаем UI:
         *
         * "операция закончена".
         *
         * Позже, когда вынесем cache в отдельный модуль,
         * именно worker будет явно вызывать fm_cache_refresh().
         */

        switch (cmd->type) {

            case FM_CMD_ENTER_VOLUME:
            case FM_CMD_ENTER_DIR:
            case FM_CMD_GO_UP:
            case FM_CMD_CREATE_FILE:
            case FM_CMD_CREATE_DIR:
            case FM_CMD_DELETE:
            case FM_CMD_RENAME:

                send_event(
                    FM_EVT_CACHE_UPDATED,
                    ESP_OK
                );

                break;

            default:
                break;
        }

    } else {

        ESP_LOGW(
            TAG,
            "Command failed: type=%d error=%s",
            cmd->type,
            esp_err_to_name(err)
        );

        send_event(
            FM_EVT_ERROR,
            err
        );
    }
}


// ============================================================================
// Worker task
// ============================================================================

static void fm_worker_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "FM worker task started");

    fm_worker_cmd_t cmd;

    while (true) {

        /*
         * Здесь worker МОЖЕТ ждать бесконечно.
         *
         * Это нормально:
         *
         * worker специально создан для этого.
         *
         * UI при этом вообще не блокируется.
         */
        if (xQueueReceive(
                s_cmd_queue,
                &cmd,
                portMAX_DELAY
            ) == pdTRUE) {

            process_command(&cmd);
        }
    }
}


// ============================================================================
// Initialization
// ============================================================================

esp_err_t fm_worker_init(void)
{
    if (s_worker_task != NULL) {
        ESP_LOGW(TAG, "FM worker already initialized");
        return ESP_OK;
    }


    // ------------------------------------------------------------------------
    // Command queue
    // ------------------------------------------------------------------------

    s_cmd_queue = xQueueCreate(
        FM_WORKER_QUEUE_LEN,
        sizeof(fm_worker_cmd_t)
    );

    if (s_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return ESP_ERR_NO_MEM;
    }


    // ------------------------------------------------------------------------
    // Event queue
    // ------------------------------------------------------------------------

    s_event_queue = xQueueCreate(
        FM_WORKER_EVENT_QUEUE_LEN,
        sizeof(fm_worker_event_t)
    );

    if (s_event_queue == NULL) {

        ESP_LOGE(TAG, "Failed to create event queue");

        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;

        return ESP_ERR_NO_MEM;
    }


    // ------------------------------------------------------------------------
    // Worker task
    // ------------------------------------------------------------------------

    BaseType_t result = xTaskCreate(
        fm_worker_task,
        "fm_worker",
        FM_WORKER_TASK_STACK,
        NULL,
        FM_WORKER_TASK_PRIORITY,
        &s_worker_task
    );

    if (result != pdPASS) {

        ESP_LOGE(TAG, "Failed to create FM worker task");

        vQueueDelete(s_event_queue);
        vQueueDelete(s_cmd_queue);

        s_event_queue = NULL;
        s_cmd_queue = NULL;

        return ESP_ERR_NO_MEM;
    }


    ESP_LOGI(
        TAG,
        "FM worker initialized: cmd=%d event=%d stack=%d",
        FM_WORKER_QUEUE_LEN,
        FM_WORKER_EVENT_QUEUE_LEN,
        FM_WORKER_TASK_STACK
    );

    return ESP_OK;
}


// ============================================================================
// Queue send helper
// ============================================================================

static esp_err_t send_command(const fm_worker_cmd_t *cmd)
{
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * НИКАКОГО portMAX_DELAY.
     *
     * UI не должен ждать worker.
     *
     * Если очередь заполнена — сразу возвращаем ошибку.
     */
    if (xQueueSend(
            s_cmd_queue,
            cmd,
            0
        ) != pdTRUE) {

        ESP_LOGW(TAG, "Command queue full");

        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}


// ============================================================================
// Public command API
// ============================================================================

esp_err_t fm_worker_send_enter_volume(uint8_t volume_index)
{
    fm_worker_cmd_t cmd = {
        .type = FM_CMD_ENTER_VOLUME,
    };

    cmd.data.volume_index = volume_index;

    return send_command(&cmd);
}


esp_err_t fm_worker_send_enter_dir(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fm_worker_cmd_t cmd = {
        .type = FM_CMD_ENTER_DIR,
    };

    strncpy(
        cmd.data.single_name.name,
        name,
        sizeof(cmd.data.single_name.name) - 1
    );

    cmd.data.single_name.name[
        sizeof(cmd.data.single_name.name) - 1
    ] = '\0';

    return send_command(&cmd);
}


esp_err_t fm_worker_send_go_up(void)
{
    fm_worker_cmd_t cmd = {
        .type = FM_CMD_GO_UP,
    };

    return send_command(&cmd);
}


esp_err_t fm_worker_send_create_file(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fm_worker_cmd_t cmd = {
        .type = FM_CMD_CREATE_FILE,
    };

    strncpy(
        cmd.data.single_name.name,
        name,
        sizeof(cmd.data.single_name.name) - 1
    );

    cmd.data.single_name.name[
        sizeof(cmd.data.single_name.name) - 1
    ] = '\0';

    return send_command(&cmd);
}


esp_err_t fm_worker_send_create_dir(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fm_worker_cmd_t cmd = {
        .type = FM_CMD_CREATE_DIR,
    };

    strncpy(
        cmd.data.single_name.name,
        name,
        sizeof(cmd.data.single_name.name) - 1
    );

    cmd.data.single_name.name[
        sizeof(cmd.data.single_name.name) - 1
    ] = '\0';

    return send_command(&cmd);
}


esp_err_t fm_worker_send_delete(
    const char *name,
    bool is_dir
)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fm_worker_cmd_t cmd = {
        .type = FM_CMD_DELETE,
    };

    strncpy(
        cmd.data.delete_entry.name,
        name,
        sizeof(cmd.data.delete_entry.name) - 1
    );

    cmd.data.delete_entry.name[
        sizeof(cmd.data.delete_entry.name) - 1
    ] = '\0';

    cmd.data.delete_entry.is_dir = is_dir;

    return send_command(&cmd);
}


esp_err_t fm_worker_send_rename(
    const char *old_name,
    const char *new_name
)
{
    if (old_name == NULL || new_name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fm_worker_cmd_t cmd = {
        .type = FM_CMD_RENAME,
    };

    strncpy(
        cmd.data.rename.old_name,
        old_name,
        sizeof(cmd.data.rename.old_name) - 1
    );

    cmd.data.rename.old_name[
        sizeof(cmd.data.rename.old_name) - 1
    ] = '\0';


    strncpy(
        cmd.data.rename.new_name,
        new_name,
        sizeof(cmd.data.rename.new_name) - 1
    );

    cmd.data.rename.new_name[
        sizeof(cmd.data.rename.new_name) - 1
    ] = '\0';

    return send_command(&cmd);
}


esp_err_t fm_worker_send_open_file(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fm_worker_cmd_t cmd = {
        .type = FM_CMD_OPEN_FILE,
    };

    strncpy(
        cmd.data.file.path,
        path,
        sizeof(cmd.data.file.path) - 1
    );

    cmd.data.file.path[
        sizeof(cmd.data.file.path) - 1
    ] = '\0';

    return send_command(&cmd);
}


esp_err_t fm_worker_send_save_file(const char *path)
{
    if (path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fm_worker_cmd_t cmd = {
        .type = FM_CMD_SAVE_FILE,
    };

    strncpy(
        cmd.data.file.path,
        path,
        sizeof(cmd.data.file.path) - 1
    );

    cmd.data.file.path[
        sizeof(cmd.data.file.path) - 1
    ] = '\0';

    return send_command(&cmd);
}


// ============================================================================
// Worker -> UI
// ============================================================================

bool fm_worker_receive_event(fm_worker_event_t *event)
{
    if (event == NULL || s_event_queue == NULL) {
        return false;
    }

    /*
     * UI никогда не ждёт здесь.
     *
     * Проверяем очередь и сразу возвращаем управление.
     */
    return xQueueReceive(
        s_event_queue,
        event,
        0
    ) == pdTRUE;
}
