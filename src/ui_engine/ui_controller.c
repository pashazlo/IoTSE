#include "ui_controller.h"

#include <string.h>

#include "ui_screen.h"
#include "ui_menu.h"
#include "ui_focus.h"
#include "ui_render.h"
#include "ui_keyboard.h"

#include "fm.h"
#include "fm_text_edit.h"

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
                fm_create_dir(text);
            }
            break;

        case KB_PURPOSE_NEW_FILE:
            if (text[0] != '\0') {
                fm_create_file(text);
            }
            break;

        case KB_PURPOSE_RENAME:
            if (text[0] == '\0') {
                // Пустое имя при переименовании — это сигнал "удалить".
                // Так на 5 кнопках без отдельной клавиши Delete можно
                // и переименовать, и удалить одним и тем же жестом (RIGHT).
                fm_delete_entry(s_kb_target_name, s_kb_target_is_dir);
            } else if (strcmp(text, s_kb_target_name) != 0) {
                fm_rename(s_kb_target_name, text);
            }
            break;

        case KB_PURPOSE_EDIT_LINE:
            fm_text_edit_set_line(s_kb_target_line, text);
            break;

        default:
            break;
    }

    s_kb_purpose = KB_PURPOSE_NONE;

    // Содержимое директории могло измениться (создали/удалили/
    // переименовали) — индекс фокуса мог указывать теперь "в никуда".
    if (ui_screen_get() == UI_SCREEN_FILE_BROWSER) {
        ui_focus_reset(UI_FOCUS_FILE_BROWSER);
    }
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
            // При неудаче (том недоступен, например SD не вставлена)
            // остаёмся на месте — рендер сам решает, как это показать.
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
                        fm_build_full_path(entry->name, s_editor_path, sizeof(s_editor_path));
                        if (fm_text_edit_open(s_editor_path) == ESP_OK) {
                            ui_focus_reset(UI_FOCUS_FILE_EDITOR);
                            ui_screen_set(UI_SCREEN_FILE_EDITOR);
                        }
                    }
                }
            }

            ui_render(canvas);
            break;
        }

        case UI_EVT_RIGHT: {

            // Переименовать/удалить — только для настоящих записей,
            // не для синтетических пунктов "[+ New ...]".
            uint8_t sel = ui_focus_get(UI_FOCUS_FILE_BROWSER);

            if (sel >= FM_BROWSER_SYNTH_COUNT) {

                const fm_entry_t *entry = fm_get_cached_entry(sel - FM_BROWSER_SYNTH_COUNT);

                if (entry != NULL) {
                    s_kb_purpose = KB_PURPOSE_RENAME;
                    s_kb_target_is_dir = entry->is_dir;

                    strncpy(s_kb_target_name, entry->name, sizeof(s_kb_target_name) - 1);
                    s_kb_target_name[sizeof(s_kb_target_name) - 1] = '\0';

                    ui_keyboard_open(entry->name);
                }
            }

            ui_render(canvas);
            break;
        }

        case UI_EVT_LEFT:

            if (!fm_go_up()) {
                // Уже в корне тома — на экран выбора тома.
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
// Экран редактора текста
// ============================================================================

static void handle_file_editor_event(ui_event_t evt, gfx_canvas_t *canvas)
{
    uint8_t count = (uint8_t)fm_text_edit_line_count();

    switch (evt) {

        case UI_EVT_UP:
        case UI_EVT_DOWN:
            ui_focus_move(UI_FOCUS_FILE_EDITOR, count, evt);
            ui_render(canvas);
            break;

        case UI_EVT_SELECT: {
            uint16_t sel = ui_focus_get(UI_FOCUS_FILE_EDITOR);
            s_kb_purpose = KB_PURPOSE_EDIT_LINE;
            s_kb_target_line = sel;
            ui_keyboard_open(fm_text_edit_get_line(sel));
            ui_render(canvas);
            break;
        }

        case UI_EVT_RIGHT: {
            // Новая пустая строка сразу после текущей, курсор — на неё.
            uint16_t sel = ui_focus_get(UI_FOCUS_FILE_EDITOR);
            fm_text_edit_insert_line_after(sel, "");
            ui_focus_move(UI_FOCUS_FILE_EDITOR, (uint8_t)fm_text_edit_line_count(), UI_EVT_DOWN);
            ui_render(canvas);
            break;
        }

        case UI_EVT_LEFT:
            // Сохраняем и возвращаемся к списку файлов
            fm_text_edit_save(s_editor_path);
            fm_text_edit_close();
            ui_screen_set(UI_SCREEN_FILE_BROWSER);
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
    // Клавиатура — модальная поверх всего. Пока она открыта, любые
    // события идут ТОЛЬКО в неё, обычный контроллер экрана не видит их.
    if (ui_keyboard_is_open()) {

        ui_keyboard_handle_event(evt);

        if (!ui_keyboard_is_open()) {
            // Закрылась именно в этом вызове — разобрать результат.
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
            handle_file_editor_event(evt, canvas);
            return;

        default:
            break;
    }

    const ui_menu_screen_t *menu = ui_menu_get_screen(screen);

    if (menu == NULL) {
        // Сплэш либо ещё не описанный экран — обрабатывать нечем.
        return;
    }

    handle_menu_event(evt, canvas, menu);
}
