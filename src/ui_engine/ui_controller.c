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
    KB_PURPOSE_EDIT_LINE,
} kb_purpose_t;

static kb_purpose_t s_kb_purpose = KB_PURPOSE_NONE;
static char s_kb_target_name[FM_MAX_NAME_LEN];  // для RENAME — старое имя
static bool s_kb_target_is_dir = false;          // для RENAME — файл или папка
static uint16_t s_kb_target_line = 0;            // для EDIT_LINE — индекс строки

// Путь к файлу, открытому в редакторе — нужен на LEFT (сохранить и выйти).
static char s_editor_path[FM_MAX_PATH_LEN];

// Синтетические пункты в начале списка браузера файлов —
// не настоящие файлы, а команды "создать новое".
#define FM_BROWSER_SYNTH_NEW_FOLDER   0
#define FM_BROWSER_SYNTH_NEW_FILE     1
#define FM_BROWSER_SYNTH_COUNT        2


/* После изменения кэша индекс может исчезнуть или относиться к другому имени. */
static void browser_clamp_focus(void)
{
    uint8_t count = fm_get_cached_count();
    uint8_t selected = ui_focus_get(UI_FOCUS_FILE_BROWSER);
    uint8_t total = count + FM_BROWSER_SYNTH_COUNT;
    if (count == 0) selected = FM_BROWSER_SYNTH_NEW_FOLDER;
    else if (selected >= total) selected = total - 1;
    ui_focus_set(UI_FOCUS_FILE_BROWSER, selected);
}

/* fm.c сортирует кэш после rename/create: ищем объект по полному имени. */
static void browser_select_name(const char *name)
{
    uint8_t count = fm_get_cached_count();
    for (uint8_t i = 0; i < count; i++) {
        const fm_entry_t *entry = fm_get_cached_entry(i);
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
                if (fm_create_dir(text) == ESP_OK) browser_select_name(text);
            }
            break;

        case KB_PURPOSE_NEW_FILE:
            if (text[0] != '\0') {
                if (fm_create_file(text) == ESP_OK) browser_select_name(text);
            }
            break;

        case KB_PURPOSE_RENAME:
            if (text[0] != '\0' && strcmp(text, s_kb_target_name) != 0) {
                esp_err_t err = fm_rename(s_kb_target_name, text);
                if (err != ESP_OK) {
                    ESP_LOGE("UI", "Rename failed: %s", esp_err_to_name(err));
                    ui_popup_show_error("Rename failed");
                } else {
                    browser_select_name(text);
                }
            }
            break;

        case KB_PURPOSE_EDIT_LINE:
            fm_text_edit_set_line(s_kb_target_line, text);
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
    uint8_t count = fm_volume_count();

    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:
            ui_focus_move(UI_FOCUS_FILE_VOLUMES, count, evt);
            ui_render(canvas);
            break;

        case UI_EVT_SELECT: {
            uint8_t sel = ui_focus_get(UI_FOCUS_FILE_VOLUMES);
            if (fm_enter_volume(sel) == ESP_OK) {
                ui_focus_reset(UI_FOCUS_FILE_BROWSER);
                ui_screen_set(UI_SCREEN_FILE_BROWSER);
            }
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
    uint8_t real_count = fm_get_cached_count();
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

                const fm_entry_t *entry = fm_get_cached_entry(sel - FM_BROWSER_SYNTH_COUNT);

                if (entry != NULL) {
                    if (entry->is_dir) {
                        if (fm_enter_dir(entry->name) == ESP_OK) {
                            ui_focus_reset(UI_FOCUS_FILE_BROWSER);
                        }
                    } else {
                       esp_err_t path_err = fm_build_full_path(entry->name, s_editor_path, sizeof(s_editor_path));

                                            if (path_err == ESP_OK && ui_file_editor_open_file(s_editor_path)) {
                        ui_focus_reset(UI_FOCUS_FILE_EDITOR);

                                                    ui_screen_set(UI_SCREEN_FILE_EDITOR);
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
                const fm_entry_t *entry = fm_get_cached_entry(sel - FM_BROWSER_SYNTH_COUNT);
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

            if (!fm_go_up()) {
                ui_screen_set(UI_SCREEN_FILE_VOLUMES);
            } else {
                ui_focus_reset(UI_FOCUS_FILE_BROWSER);
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
        if (result == UI_POPUP_RENAME) {
            s_kb_purpose = KB_PURPOSE_RENAME;
            ui_keyboard_open(s_kb_target_name);
        } else if (result == UI_POPUP_DELETE) {
            esp_err_t err = fm_delete_entry(s_kb_target_name, s_kb_target_is_dir);
            if (err == ESP_OK) {
                /* Прежний индекс теперь указывает на следующую запись.
                   Если удалили последнюю — выбираем предыдущую. */
                browser_clamp_focus();
            } else {
                ESP_LOGE("UI", "Delete failed: %s", esp_err_to_name(err));
                ui_popup_show_error("Delete failed");
            }
        }
        ui_render(canvas);
        return;
    }

    if (ui_keyboard_is_open()) {

        ui_keyboard_handle_event(evt);

        if (!ui_keyboard_is_open()) {
            handle_keyboard_result();
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
            switch (evt) {
                case UI_EVT_UP:
                case UI_EVT_DOWN:
                    ui_focus_move(UI_FOCUS_FILE_EDITOR,
                                  (uint8_t)fm_text_edit_line_count(), evt);
                    break;
                case UI_EVT_SELECT:
                    s_kb_target_line = ui_focus_get(UI_FOCUS_FILE_EDITOR);
                    s_kb_purpose = KB_PURPOSE_EDIT_LINE;
                    ui_keyboard_open(fm_text_edit_get_line(s_kb_target_line));
                    break;
                case UI_EVT_LEFT:
                    if (fm_text_edit_save()) {
                        fm_text_edit_close();
                        ui_screen_set(UI_SCREEN_FILE_BROWSER);
                    } else {
                        ESP_LOGE("UI", "Save failed; document retained in editor");
                    }
                    break;
                default:
                    break;
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
