#include "ui_controller.h"

#include <string.h>

#include "ui_screen.h"
#include "ui_menu.h"
#include "ui_focus.h"
#include "ui_render.h"
#include "ui_keyboard.h"
#include "ui_popup.h"
#include "ui_file_editor.h"

#include "fm.h"
#include "fm_worker.h"
#include "fm_text_edit.h"
#include "esp_log.h"

// ============================================================================
// Зачем открыта клавиатура — контекст, чтобы знать, что делать
// с введённым текстом после того, как она закроется по OK.
// ============================================================================

typedef enum {
    KB_PURPOSE_NONE = 0,
    KB_PURPOSE_NEW_FOLDER,
    KB_PURPOSE_NEW_FILE,
    KB_PURPOSE_RENAME,
} kb_purpose_t;

static kb_purpose_t s_kb_purpose = KB_PURPOSE_NONE;
static char s_kb_target_name[FM_MAX_NAME_LEN];  // для RENAME — старое имя
static bool s_kb_target_is_dir = false;          // для RENAME — файл или папка

// Путь к файлу, открытому в редакторе — нужен на LEFT (сохранить и выйти).
static char s_editor_path[FM_MAX_PATH_LEN];
static fm_worker_cmd_type_t s_pending_command = FM_CMD_NONE;
static char s_pending_focus_name[FM_MAX_NAME_LEN];

// Синтетические пункты в начале списка браузера файлов —
// не настоящие файлы, а команды "создать новое".
#define FM_BROWSER_SYNTH_NEW_FOLDER   0
#define FM_BROWSER_SYNTH_NEW_FILE     1
#define FM_BROWSER_SYNTH_COUNT        2

static bool worker_start(esp_err_t result, fm_worker_cmd_type_t command)
{
    if (result != ESP_OK) {
        ui_popup_show_error("Worker busy");
        return false;
    }
    s_pending_command = command;
    switch (command) {
        case FM_CMD_ENTER_VOLUME:
        case FM_CMD_ENTER_DIR:
        case FM_CMD_GO_UP:
        case FM_CMD_OPEN_FILE:
            ui_render_set_worker_status("Opening...");
            break;
        case FM_CMD_CREATE_FILE:
        case FM_CMD_CREATE_DIR:
            ui_render_set_worker_status("Creating...");
            break;
        case FM_CMD_RENAME:
            ui_render_set_worker_status("Renaming...");
            break;
        case FM_CMD_DELETE:
            ui_render_set_worker_status("Deleting...");
            break;
        default:
            ui_render_set_worker_status("Working...");
            break;
    }
    return true;
}

static bool browser_is_at_volume_root(void)
{
    const char *path = fm_current_path();
    for (uint8_t i = 0; i < fm_volume_count(); i++) {
        const fm_volume_t *volume = fm_get_volume(i);
        if (volume != NULL && volume->mount_point != NULL &&
            strcmp(path, volume->mount_point) == 0) {
            return true;
        }
    }
    return false;
}


/* После изменения кэша индекс может исчезнуть или относиться к другому имени. */
static void browser_clamp_focus(void)
{
    fm_cache_snapshot_t snapshot;
    fm_get_cache_snapshot(&snapshot);
    uint8_t count = snapshot.count;
    uint8_t selected = ui_focus_get(UI_FOCUS_FILE_BROWSER);
    uint8_t total = count + FM_BROWSER_SYNTH_COUNT;
    if (count == 0) selected = FM_BROWSER_SYNTH_NEW_FOLDER;
    else if (selected >= total) selected = total - 1;
    ui_focus_set(UI_FOCUS_FILE_BROWSER, selected);
}

/* fm.c сортирует кэш после rename/create: ищем объект по полному имени. */
static void browser_select_name(const char *name)
{
    fm_cache_snapshot_t snapshot;
    fm_get_cache_snapshot(&snapshot);
    uint8_t count = snapshot.count;
    for (uint8_t i = 0; i < count; i++) {
        const fm_entry_t *entry = &snapshot.entries[i];
        if (entry && strcmp(entry->name, name) == 0) {
            ui_focus_set(UI_FOCUS_FILE_BROWSER, i + FM_BROWSER_SYNTH_COUNT);
            return;
        }
    }
    browser_clamp_focus();
}

// ============================================================================
// Разбор результата клавиатуры — вызывается ровно один раз, сразу
// после того как ui_keyboard_is_open() стала false.
// ============================================================================

static void handle_keyboard_result(void)
{
    if (!ui_keyboard_was_confirmed()) {
        // Отмена по ESC — ничего не делаем, просто забываем контекст.
        s_kb_purpose = KB_PURPOSE_NONE;
        return;
    }

    const char *text = ui_keyboard_get_text();

    switch (s_kb_purpose) {

        case KB_PURPOSE_NEW_FOLDER:
            if (text[0] != '\0') {
                strncpy(s_pending_focus_name, text, sizeof(s_pending_focus_name) - 1);
                s_pending_focus_name[sizeof(s_pending_focus_name) - 1] = '\0';
                worker_start(fm_worker_send_create_dir(text), FM_CMD_CREATE_DIR);
            }
            break;

        case KB_PURPOSE_NEW_FILE:
            if (text[0] != '\0') {
                strncpy(s_pending_focus_name, text, sizeof(s_pending_focus_name) - 1);
                s_pending_focus_name[sizeof(s_pending_focus_name) - 1] = '\0';
                worker_start(fm_worker_send_create_file(text), FM_CMD_CREATE_FILE);
            }
            break;

        case KB_PURPOSE_RENAME:
            if (text[0] != '\0' && strcmp(text, s_kb_target_name) != 0) {
                strncpy(s_pending_focus_name, text, sizeof(s_pending_focus_name) - 1);
                s_pending_focus_name[sizeof(s_pending_focus_name) - 1] = '\0';
                worker_start(
                    fm_worker_send_rename(s_kb_target_name, text),
                    FM_CMD_RENAME
                );
            }
            break;

        default:
            break;
    }

    s_kb_purpose = KB_PURPOSE_NONE;

}


// ============================================================================
// Экран выбора тома (Internal / SD Card)
// ============================================================================

static void handle_file_volumes_event(ui_event_t evt, gfx_canvas_t *canvas)
{
    if (s_pending_command != FM_CMD_NONE) {
        return;
    }

    uint8_t count = fm_volume_count();

    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:
            ui_focus_move(UI_FOCUS_FILE_VOLUMES, count, evt);
            ui_render(canvas);
            break;

        case UI_EVT_SELECT: {
            uint8_t sel = ui_focus_get(UI_FOCUS_FILE_VOLUMES);
            worker_start(fm_worker_send_enter_volume(sel), FM_CMD_ENTER_VOLUME);
            ui_render(canvas);
            break;
        }

        case UI_EVT_LEFT:
            action_back_to_main();
            ui_render(canvas);
            break;

        default:
            break;
    }
}


// ============================================================================
// Экран браузера файлов
// ============================================================================

static void handle_file_browser_event(ui_event_t evt, gfx_canvas_t *canvas)
{
    if (s_pending_command != FM_CMD_NONE) {
        return;
    }

    fm_cache_snapshot_t snapshot;
    fm_get_cache_snapshot(&snapshot);
    uint8_t real_count = snapshot.count;
    uint8_t total_count = real_count + FM_BROWSER_SYNTH_COUNT;

    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:
            ui_focus_move(UI_FOCUS_FILE_BROWSER, total_count, evt);
            ui_render(canvas);
            break;

        case UI_EVT_SELECT: {

            uint8_t sel = ui_focus_get(UI_FOCUS_FILE_BROWSER);

            if (sel == FM_BROWSER_SYNTH_NEW_FOLDER) {
                s_kb_purpose = KB_PURPOSE_NEW_FOLDER;
                ui_keyboard_open("");

            } else if (sel == FM_BROWSER_SYNTH_NEW_FILE) {
                s_kb_purpose = KB_PURPOSE_NEW_FILE;
                ui_keyboard_open("");

            } else {

                const fm_entry_t *entry =
                    &snapshot.entries[sel - FM_BROWSER_SYNTH_COUNT];

                if (entry != NULL) {
                    if (entry->is_dir) {
                        worker_start(
                            fm_worker_send_enter_dir(entry->name),
                            FM_CMD_ENTER_DIR
                        );
                    } else {
                        esp_err_t path_err = fm_build_full_path(
                            entry->name,
                            s_editor_path,
                            sizeof(s_editor_path)
                        );
                        if (path_err == ESP_OK) {
                            esp_err_t send_err = fm_worker_send_open_file(s_editor_path);
                            worker_start(send_err, FM_CMD_OPEN_FILE);
                        }
                    }
                }
            }

            ui_render(canvas);
            break;
        }

        case UI_EVT_CONTEXT: {
            uint8_t sel = ui_focus_get(UI_FOCUS_FILE_BROWSER);
            if (sel >= FM_BROWSER_SYNTH_COUNT) {
                const fm_entry_t *entry =
                    &snapshot.entries[sel - FM_BROWSER_SYNTH_COUNT];
                if (entry) {
                    /* Snapshot target: popup navigation must not change browser focus. */
                    strncpy(s_kb_target_name, entry->name, sizeof(s_kb_target_name) - 1);
                    s_kb_target_name[sizeof(s_kb_target_name) - 1] = '\0';
                    s_kb_target_is_dir = entry->is_dir;
                    ui_popup_open(s_kb_target_name, s_kb_target_is_dir);
                }
            }
            ui_render(canvas);
            break;
        }

        case UI_EVT_LEFT:
            if (browser_is_at_volume_root()) {
                ui_screen_set(UI_SCREEN_FILE_VOLUMES);
            } else {
                worker_start(fm_worker_send_go_up(), FM_CMD_GO_UP);
            }

            ui_render(canvas);
            break;

        default:
            break;
    }
}




// ============================================================================
// Обычные меню (главное меню и статические подменю)
// ============================================================================

static void handle_menu_event(ui_event_t evt, gfx_canvas_t *canvas, const ui_menu_screen_t *menu)
{
    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:
            ui_focus_move(menu->focus_id, menu->count, evt);
            ui_render(canvas);
            break;

        case UI_EVT_SELECT: {
            uint8_t selected = ui_focus_get(menu->focus_id);
            if (menu->items[selected].callback) {
                menu->items[selected].callback();
            }
            ui_render(canvas);
            break;
        }

        default:
            break;
    }
}


// ============================================================================
// Точка входа
// ============================================================================

void ui_controller_handle_event(
    ui_event_t evt,
    gfx_canvas_t *canvas
)
{
    /* Modal dispatch comes before every underlying screen handler. */
    if (ui_popup_is_open()) {
        ui_popup_result_t result = ui_popup_handle_event(evt);
        if (ui_screen_get() == UI_SCREEN_FILE_EDITOR) {
            if (ui_file_editor_handle_popup_result(result)) {
                ui_screen_set(UI_SCREEN_FILE_BROWSER);
            }
        } else if (result == UI_POPUP_RENAME) {
            s_kb_purpose = KB_PURPOSE_RENAME;
            ui_keyboard_open(s_kb_target_name);
        } else if (result == UI_POPUP_DELETE) {
            worker_start(
                fm_worker_send_delete(s_kb_target_name, s_kb_target_is_dir),
                FM_CMD_DELETE
            );
        }
        ui_render(canvas);
        return;
    }

    if (ui_keyboard_is_open()) {

        ui_keyboard_handle_event(evt);

        if (!ui_keyboard_is_open()) {
            if (ui_screen_get() == UI_SCREEN_FILE_EDITOR) {
                ui_file_editor_handle_keyboard_result();
            } else {
                handle_keyboard_result();
            }
        }

        ui_render(canvas);
        return;
    }

    ui_screen_t screen = ui_screen_get();

    switch (screen) {

        case UI_SCREEN_FILE_VOLUMES:
            handle_file_volumes_event(evt, canvas);
            return;

        case UI_SCREEN_FILE_BROWSER:
            handle_file_browser_event(evt, canvas);
            return;

        case UI_SCREEN_FILE_EDITOR:
            if (evt == UI_EVT_CONTEXT) {
                ui_file_editor_open_actions();
            } else {
                if (ui_file_editor_handle_event(evt)) {
                    ui_screen_set(UI_SCREEN_FILE_BROWSER);
                }
            }
            ui_render(canvas);
            return;

        default:
            break;
    }

    const ui_menu_screen_t *menu = ui_menu_get_screen(screen);

    if (menu == NULL) {
        return;
    }

    handle_menu_event(evt, canvas, menu);
}

// Worker replies are consumed without blocking the UI task.
void ui_controller_poll_worker(gfx_canvas_t *canvas)
{
    fm_worker_event_t event;
    while (fm_worker_receive_event(&event)) {
        bool render_needed = false;

        if (event.command == FM_CMD_OPEN_FILE &&
            s_pending_command == FM_CMD_OPEN_FILE) {
            s_pending_command = FM_CMD_NONE;
            ui_render_set_worker_status(NULL);
            if (event.type == FM_EVT_FILE_LOADED) {
                ui_file_editor_open_loaded();
                ui_focus_reset(UI_FOCUS_FILE_EDITOR);
                ui_screen_set(UI_SCREEN_FILE_EDITOR);
            } else if (event.type == FM_EVT_ERROR) {
                ui_popup_show_error("Open failed");
            }
            render_needed = true;
        } else if (event.command == FM_CMD_SAVE_FILE) {
            if (ui_file_editor_handle_worker_event(&event)) {
                ui_screen_set(UI_SCREEN_FILE_BROWSER);
            }
            render_needed = true;
        } else if (event.command == s_pending_command &&
                   event.type == FM_EVT_CACHE_UPDATED) {
            fm_worker_cmd_type_t completed = s_pending_command;
            s_pending_command = FM_CMD_NONE;
            ui_render_set_worker_status(NULL);

            switch (completed) {
                case FM_CMD_ENTER_VOLUME:
                    ui_focus_reset(UI_FOCUS_FILE_BROWSER);
                    ui_screen_set(UI_SCREEN_FILE_BROWSER);
                    break;
                case FM_CMD_ENTER_DIR:
                case FM_CMD_GO_UP:
                    ui_focus_reset(UI_FOCUS_FILE_BROWSER);
                    break;
                case FM_CMD_CREATE_FILE:
                case FM_CMD_CREATE_DIR:
                case FM_CMD_RENAME:
                    browser_select_name(s_pending_focus_name);
                    break;
                case FM_CMD_DELETE:
                    browser_clamp_focus();
                    break;
                default:
                    break;
            }
            render_needed = true;
        } else if (event.command == s_pending_command &&
                   event.type == FM_EVT_ERROR) {
            s_pending_command = FM_CMD_NONE;
            ui_render_set_worker_status(NULL);
            ui_popup_show_error("FM operation failed");
            render_needed = true;
        }

        if (event.type == FM_EVT_ERROR) {
            ESP_LOGE("UI", "FM worker command=%d error=%s",
                     (int)event.command, esp_err_to_name(event.error));
        } else {
            ESP_LOGI("UI", "FM worker command=%d event=%d",
                     (int)event.command, (int)event.type);
        }

        if (render_needed && canvas != NULL) {
            ui_render(canvas);
        }
    }
}

bool ui_controller_worker_pending(void)
{
    return s_pending_command != FM_CMD_NONE;
}
