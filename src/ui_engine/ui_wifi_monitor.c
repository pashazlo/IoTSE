#include "ui_wifi_monitor.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "assets/ibm_vga_font.h"
#include "display.h"
#include "ui_cursor.h"
#include "ui_focus.h"
#include "ui_popup.h"
#include "ui_render.h"
#include "ui_screen.h"
#include "wifi_analyzer.h"
#include "wifi_ap_tracker.h"
#include "wifi_capture_store.h"
#include "wifi_channel_hopper.h"
#include "wifi_eapol_tracker.h"
#include "wifi_pmkid_store.h"   /* счётчик пойманных PMKID для статистики */
#include "wifi_pmkid_writer.h"  /* результат автосохранения PMKID на SD */
#include "wifi_promiscuous.h"
#include "wifi_sta_tracker.h"
#include "tx_engine.h"
#include "tx_config.h" /* настройки пакета (SSID/reason/burst) со страницы TX ESP */

#define UI_FONT (&Px437_IBM_VGA_8x14_2x8pt7b)
#define GRAPH_SAMPLES 28
#define GRAPH_SAMPLE_MS 200U
#define DEFAULT_DWELL_MS 250U
#define MONITOR_PRIMARY_ACTION_COUNT 5U
#define MONITOR_SECONDARY_ACTION_COUNT 2U
/* TX-страница: 5 фокусируемых элементов — Send beacon(0), Send deauth(1),
 * Get PMKID(2), BACK(3), FRONT(4). Страница только под функции-кнопки (без
 * RX-статистики). */
#define MONITOR_TX_ACTION_COUNT 5U
/* SSID beacon, код причины deauth и размер burst больше НЕ зашиты здесь — их
 * задаёт пользователь на странице TX ESP, а мы читаем из модели tx_config в
 * момент отправки (tx_config_ssid / tx_config_deauth_reason / tx_config_burst_count). */
/* Сколько строк AP помещается в окне выбора цели deauth. */
#define DEAUTH_PICKER_VISIBLE_ROWS 8U
#define MONITOR_SLIDE_MS 200U
#define MONITOR_FOCUS_LEFT_X 7
#define MONITOR_FOCUS_LEFT_W 146
#define MONITOR_FOCUS_RIGHT_X 166
#define MONITOR_FOCUS_RIGHT_W 147

/* Порядок значений = порядок страниц в цепочке и направление слайда:
 * PRIMARY(0) → SECONDARY(1) → TX(2). Переход к большему индексу едет «вперёд». */
typedef enum {
    MONITOR_PAGE_PRIMARY,
    MONITOR_PAGE_SECONDARY,
    MONITOR_PAGE_TX,
} monitor_page_t;

/* Переход между страницами как слайд от страницы from к странице to.
 * Раньше здесь был булев forward (только PRIMARY<->SECONDARY). Обобщили до пары
 * from/to, чтобы поддержать цепочку из нескольких страниц, не меняя саму
 * анимацию: направление слайда определяется порядком значений в monitor_page_t
 * (переход к большему индексу = новая страница въезжает справа). */
typedef struct {
    bool active;
    monitor_page_t from;
    monitor_page_t to;
    TickType_t started_at;
} monitor_transition_t;

typedef enum {
    MONITOR_IDLE,
    MONITOR_STARTING_RADIO,
    MONITOR_WAITING_FOR_IDLE,
    MONITOR_DISCONNECTING_STA,
    MONITOR_STARTING_CAPTURE,
    MONITOR_STOPPING_CAPTURE,
    MONITOR_UPDATING_HOPPER,
    MONITOR_SETTING_CHANNEL,
    MONITOR_PMKID_STOP_HOP, /* стопим хоппер перед парковкой на канал цели PMKID */
    MONITOR_PMKID_SET_CH,   /* ставим канал цели; по подтверждению — solicit PMKID */
} monitor_phase_t;

static monitor_phase_t s_phase;
static uint32_t s_request_id;
static uint32_t s_packets, s_management, s_data, s_control, s_eapol;
static ui_cursor_t s_cursor;
static bool s_start_hopper_after_capture;
static char s_storage_status[64];
static uint8_t s_manual_channel = 1;
static uint16_t s_graph[GRAPH_SAMPLES];
static uint32_t s_graph_window_packets;
static TickType_t s_graph_last_tick;
static bool s_exit_after_stop;
static wifi_pipeline_diagnostics_t s_diagnostics;
static wifi_analyzer_snapshot_t s_analyzer_snapshot;
static wifi_channel_engine_snapshot_t s_engine;
static wifi_channel_observation_t s_channels[WIFI_ANALYZER_MAX_CHANNELS];
static uint8_t s_first_channel = 1, s_last_channel = 11;
static monitor_page_t s_page;
static monitor_transition_t s_transition;
static wifi_ap_tracker_diagnostics_t s_ap_diagnostics;
static wifi_sta_tracker_diagnostics_t s_sta_diagnostics;
static uint16_t s_completed_handshakes;
static uint16_t s_pmkid_count; /* уникальных PMKID в store; кеш для статистики */
static bool s_pmkid_sd_error;  /* последняя автозапись PMKID не удалась (нет SD) */
static bool s_analyzer_snapshot_valid;
static bool s_ap_snapshot_valid;
static bool s_sta_snapshot_valid;
static bool s_handshake_snapshot_valid;

/* Модальное окно выбора цели из списка увиденных AP (как в Networks). Один и тот
 * же пикер обслуживает два действия — отправку deauth и активную добычу PMKID;
 * s_pick_action говорит, какое действие выполнить по SELECT (и PICK_NONE = окно
 * закрыто). s_pick_sel — выбранная строка; s_pick_top — первая видимая (прокрутка).
 * s_pick_aps — КОПИЯ снапшота AP-трекера: структура ~20 КБ, поэтому держим её в
 * static (а не на стеке UI-задачи, как прямо требует комментарий в
 * wifi_ap_tracker.h). Буфер общий: одновременно открыт максимум один пикер. */
typedef enum {
    PICK_NONE = 0, /* окно закрыто */
    PICK_DEAUTH,   /* SELECT → отправить broadcast-deauth выбранной точке */
    PICK_PMKID,    /* SELECT → активно спровоцировать PMKID у выбранной точки */
} pick_action_t;
static pick_action_t s_pick_action;
static uint16_t s_pick_sel;
static uint16_t s_pick_top;
static wifi_ap_tracker_snapshot_t s_pick_aps;

/* Сколько раундов провокации (auth+assoc) шлём за одно нажатие Get PMKID. Немного
 * повышает шанс поймать M1, не засыпая эфир; пользователь может нажать повторно. */
#define MONITOR_PMKID_ROUNDS 3U

/* Отложенная цель PMKID: выбрали точку, но сперва паркуем радио на её канал
 * (стоп хоппера + set_channel). Когда канал подтверждён worker'ом — шлём провокацию
 * по этой копии. Копируем всю запись AP (bssid/ssid/rsn/канал нужны разом). */
static bool s_pmkid_after_park;
static wifi_tracked_ap_t s_pmkid_target;

static bool connection_transition_active(void)
{
    wifi_worker_state_t state = wifi_worker_get_state();
    return state == WIFI_WORKER_STATE_STARTING ||
           state == WIFI_WORKER_STATE_ASSOCIATING ||
           state == WIFI_WORKER_STATE_WAITING_IP ||
           state == WIFI_WORKER_STATE_SWITCHING_NETWORK ||
           state == WIFI_WORKER_STATE_DISCONNECTING ||
           state == WIFI_WORKER_STATE_SCANNING;
}

static void format_count(char *out, size_t size, uint64_t value)
{
    if (value >= 1000000000ULL)
        snprintf(out, size, "%lluG", (unsigned long long)(value / 1000000000ULL));
    else if (value >= 1000000ULL)
        snprintf(out, size, "%lluM", (unsigned long long)(value / 1000000ULL));
    else if (value >= 1000ULL)
        snprintf(out, size, "%lluK", (unsigned long long)(value / 1000ULL));
    else
        snprintf(out, size, "%llu", (unsigned long long)value);
}

static void show_queue_error(void)
{
    s_phase = MONITOR_IDLE;
    ui_popup_show_error("Wi-Fi worker busy");
}

static void leave_monitor(void)
{
    s_exit_after_stop = false;
    wifi_worker_resume_auto_connect();
    ui_screen_set(UI_SCREEN_WIFI_MENU);
}

static void request_monitor_exit(void)
{
    if (wifi_worker_get_state() == WIFI_WORKER_STATE_PROMISCUOUS) {
        if (wifi_worker_send_disable_promiscuous(&s_request_id) != ESP_OK) {
            show_queue_error();
            return;
        }
        s_exit_after_stop = true;
        s_phase = MONITOR_STOPPING_CAPTURE;
        return;
    }
    leave_monitor();
}

static void request_capture_start(void)
{
    wifi_worker_state_t state = wifi_worker_get_state();
    if (state == WIFI_WORKER_STATE_OFF || state == WIFI_WORKER_STATE_ERROR) {
        if (wifi_worker_send_start(&s_request_id) != ESP_OK) {
            show_queue_error();
            return;
        }
        s_phase = MONITOR_STARTING_RADIO;
        return;
    }
    if (state == WIFI_WORKER_STATE_CONNECTED) {
        if (wifi_worker_send_disconnect(&s_request_id) != ESP_OK) {
            show_queue_error();
            return;
        }
        s_phase = MONITOR_DISCONNECTING_STA;
        return;
    }
    if (connection_transition_active()) {
        s_phase = MONITOR_WAITING_FOR_IDLE;
        return;
    }
    if (state != WIFI_WORKER_STATE_IDLE) {
        show_queue_error();
        return;
    }
    if (wifi_worker_send_enable_promiscuous(
            s_manual_channel, wifi_promiscuous_rx_cb, &s_request_id) != ESP_OK) {
        show_queue_error();
        return;
    }
    /* Новая сессия захвата началась — обнуляем счётчик отправленных TX-кадров,
     * чтобы «Sent: N» на TX-странице относился именно к этой сессии, а не копился
     * со всех прошлых. Точка единственная (единственный вызов enable_promiscuous),
     * поэтому сброс срабатывает ровно один раз на старт. */
    tx_engine_reset_sent_count();
    s_phase = MONITOR_STARTING_CAPTURE;
}

static void change_fixed_channel(ui_event_t event)
{
    int next = (int)s_manual_channel + (event == UI_EVT_RIGHT ? 1 : -1);
    if (next < s_first_channel) next = s_last_channel;
    if (next > s_last_channel) next = s_first_channel;
    s_manual_channel = (uint8_t)next;
    if (wifi_worker_get_state() == WIFI_WORKER_STATE_PROMISCUOUS &&
        !s_engine.running) {
        if (wifi_worker_send_set_channel(s_manual_channel, &s_request_id) != ESP_OK)
            show_queue_error();
        else
            s_phase = MONITOR_SETTING_CHANNEL;
    }
}

void ui_wifi_monitor_open(void)
{
    wifi_worker_suspend_auto_connect();
    s_phase = MONITOR_IDLE;
    s_exit_after_stop = false;
    if (wifi_capture_store_init() != ESP_OK)
        ui_popup_show_error("Capture memory failed");
    /* Лениво поднимаем задачу TX-движка (идемпотентно): нужна для страницы TX.
     * Запускаем здесь, а не в app_main, чтобы задача жила только когда реально
     * пользуемся монитором. Ошибку не считаем фатальной — сама отправка на TX
     * сообщит, если движок не готов. */
    (void)tx_engine_init();
    (void)wifi_worker_get_channel_range(&s_first_channel, &s_last_channel);
    ui_screen_set(UI_SCREEN_WIFI_MONITOR);
    s_page = MONITOR_PAGE_PRIMARY;
    s_pick_action = PICK_NONE;
    s_pmkid_after_park = false;
    memset(&s_transition, 0, sizeof(s_transition));
    ui_focus_reset(UI_FOCUS_WIFI_MONITOR);
    ui_cursor_reset(&s_cursor);
    s_graph_last_tick = xTaskGetTickCount();
}

/* Куда поставить фокус, когда «приземляемся» на страницу после перехода.
 * Сохраняет прежнее поведение для PRIMARY/SECONDARY (4 = FRONT, 0 = BACK) и
 * задаёт разумную точку для TX (1 = её BACK, чтобы случайно не нажать Send). */
static uint8_t page_landing_focus(monitor_page_t page)
{
    switch (page) {
    case MONITOR_PAGE_PRIMARY:   return 4U; /* кнопка FRONT (откуда ушли назад) */
    case MONITOR_PAGE_SECONDARY: return 0U; /* кнопка BACK */
    case MONITOR_PAGE_TX:        return 3U; /* кнопка BACK (0/1/2 — beacon/deauth/PMKID) */
    }
    return 0U;
}

static void start_page_transition(monitor_page_t target)
{
    if (s_transition.active) return;
    s_transition.active = true;
    s_transition.from = s_page;
    s_transition.to = target;
    s_transition.started_at = xTaskGetTickCount();
    ui_focus_set(UI_FOCUS_WIFI_MONITOR, page_landing_focus(target));
    ui_cursor_reset(&s_cursor);
}

static ui_bbox_t focus_cell(bool right_column, int16_t baseline_y)
{
    return (ui_bbox_t){
        right_column ? MONITOR_FOCUS_RIGHT_X : MONITOR_FOCUS_LEFT_X,
        baseline_y - 13,
        right_column ? MONITOR_FOCUS_RIGHT_W : MONITOR_FOCUS_LEFT_W,
        17,
    };
}

static void build_primary_focus_boxes(ui_bbox_t *boxes)
{
    boxes[0] = focus_cell(false, 116);
    boxes[1] = focus_cell(false, 132);
    boxes[2] = focus_cell(false, 148);
    boxes[3] = focus_cell(false, 164);
    boxes[4] = focus_cell(true, 164);
}

static void build_secondary_focus_boxes(ui_bbox_t *boxes)
{
    boxes[0] = focus_cell(false, 164);
    boxes[1] = focus_cell(true, 164);
}

static void build_tx_focus_boxes(ui_bbox_t *boxes)
{
    boxes[0] = focus_cell(false, 116); /* Send beacon */
    boxes[1] = focus_cell(false, 132); /* Send deauth */
    boxes[2] = focus_cell(false, 148); /* Get PMKID   */
    boxes[3] = focus_cell(false, 164); /* BACK  (левый футер)  */
    boxes[4] = focus_cell(true, 164);  /* FRONT (правый футер) */
}

/* Открыть модальное окно выбора цели из списка AP для действия action.
 * Условия: радио в PROMISCUOUS (иначе слать некуда) и трекер видел хотя бы одну
 * точку. Снимаем КОПИЮ снапшота AP один раз — дальше окно работает по ней, чтобы
 * список не «прыгал» под пользователем во время листания. Что произойдёт по
 * SELECT, определяет action (deauth или добыча PMKID). */
static void open_ap_picker(pick_action_t action)
{
    if (wifi_worker_get_state() != WIFI_WORKER_STATE_PROMISCUOUS) {
        ui_popup_show_error("Start capture first");
        return;
    }
    if (!wifi_ap_tracker_get_snapshot(&s_pick_aps) || s_pick_aps.count == 0) {
        ui_popup_show_error("No APs seen yet");
        return;
    }
    s_pick_action = action;
    s_pick_sel = 0;
    s_pick_top = 0;
    ui_cursor_reset(&s_cursor);
}

/* Канал цели: предпочитаем заявленный в beacon (DS Parameter), иначе — канал, на
 * котором её кадр реально приняли. */
static uint8_t pmkid_target_channel(const wifi_tracked_ap_t *ap)
{
    return ap->has_advertised_channel ? ap->advertised_channel : ap->rx_channel;
}

/* Запустить добычу PMKID по уже сохранённой цели s_pmkid_target: сначала паркуем
 * радио на её канал (кадры и приём M1 должны идти на канале точки, иначе она нас
 * не слышит, а ответ прилетает, когда хоппер ушёл). Если хоппер бежит — стопим его,
 * set_channel уйдёт в обработчике HOPPER_STOPPED; иначе сразу ставим канал. Сам
 * solicit уходит только после подтверждения канала (CHANNEL_SET). */
static void start_pmkid_solicit(void)
{
    if (s_engine.running) {
        if (wifi_worker_send_hopper_stop(&s_request_id) != ESP_OK) {
            s_pmkid_after_park = false;
            show_queue_error();
            return;
        }
        s_phase = MONITOR_PMKID_STOP_HOP;
        return;
    }
    uint8_t ch = pmkid_target_channel(&s_pmkid_target);
    if (ch < s_first_channel || ch > s_last_channel) {
        ch = s_manual_channel; /* подстраховка: канал цели вне диапазона */
    }
    if (wifi_worker_send_set_channel(ch, &s_request_id) != ESP_OK) {
        s_pmkid_after_park = false;
        show_queue_error();
        return;
    }
    s_manual_channel = ch;
    s_phase = MONITOR_PMKID_SET_CH;
}

/* Обработка событий, пока открыт пикер выбора AP. Возвращает true, если событие
 * «съедено» окном (тогда фоновая страница его не видит). По SELECT выполняет
 * действие, с которым пикер открыли (s_pick_action). */
static bool handle_ap_picker_event(ui_event_t event, gfx_canvas_t *canvas)
{
    if (s_pick_action == PICK_NONE) return false;
    uint16_t count = s_pick_aps.count;

    if (event == UI_EVT_UP && s_pick_sel > 0U) {
        s_pick_sel--;
    } else if (event == UI_EVT_DOWN && (uint16_t)(s_pick_sel + 1U) < count) {
        s_pick_sel++;
    } else if (event == UI_EVT_SELECT) {
        const wifi_tracked_ap_t *ap = &s_pick_aps.records[s_pick_sel];
        if (s_pick_action == PICK_DEAUTH) {
            /* Отправляем broadcast-deauth выбранной точке: цель = FF:FF:FF:FF:FF:FF,
             * bssid = выбранная AP. Отключит её клиентов. */
            static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
            esp_err_t err = tx_engine_request_deauth(
                ap->bssid, broadcast,
                tx_config_deauth_reason(), tx_config_burst_count());
            if (err != ESP_OK) ui_popup_show_error("Deauth queue busy");
        } else { /* PICK_PMKID */
            /* Для assoc нужен реальный SSID; у скрытой сети его нет — честно
             * сообщаем, а не шлём заведомо отбиваемый кадр. Иначе сохраняем цель
             * и паркуем радио на её канал; сама провокация уйдёт после парковки. */
            if (ap->ssid_length == 0U) {
                ui_popup_show_error("SSID hidden");
            } else {
                s_pmkid_target = *ap;      /* копия: bssid/ssid/rsn/канал цели */
                s_pmkid_after_park = true;
                s_pick_action = PICK_NONE; /* закрыть пикер до запуска парковки */
                ui_cursor_reset(&s_cursor);
                start_pmkid_solicit();
                ui_render(canvas);
                return true;
            }
        }
        s_pick_action = PICK_NONE;
        ui_cursor_reset(&s_cursor);
    } else if (event == UI_EVT_LEFT) {
        s_pick_action = PICK_NONE; /* отмена */
        ui_cursor_reset(&s_cursor);
    }

    /* Держим выбранную строку в видимом окне прокрутки. */
    if (s_pick_sel < s_pick_top)
        s_pick_top = s_pick_sel;
    else if (s_pick_sel >= (uint16_t)(s_pick_top + DEAUTH_PICKER_VISIBLE_ROWS))
        s_pick_top = (uint16_t)(s_pick_sel - DEAUTH_PICKER_VISIBLE_ROWS + 1U);

    ui_render(canvas);
    return true;
}

void ui_wifi_monitor_handle_event(ui_event_t event, gfx_canvas_t *canvas)
{
    if (s_phase != MONITOR_IDLE || s_transition.active) return;
    /* Модальное окно выбора цели перехватывает события раньше страниц. */
    if (handle_ap_picker_event(event, canvas)) return;
    uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_MONITOR);
    if (s_page == MONITOR_PAGE_SECONDARY) {
        if (event == UI_EVT_UP || event == UI_EVT_DOWN ||
            event == UI_EVT_LEFT || event == UI_EVT_RIGHT) {
            ui_bbox_t boxes[MONITOR_SECONDARY_ACTION_COUNT];
            build_secondary_focus_boxes(boxes);
            ui_focus_move_spatial(UI_FOCUS_WIFI_MONITOR, boxes,
                                  MONITOR_SECONDARY_ACTION_COUNT, event);
        } else if (event == UI_EVT_SELECT && selected == 0) {
            start_page_transition(MONITOR_PAGE_PRIMARY);
        } else if (event == UI_EVT_SELECT && selected == 1) {
            /* FRONT на secondary теперь ведёт на новую TX-страницу. */
            start_page_transition(MONITOR_PAGE_TX);
        }
        ui_render(canvas);
        return;
    }
    if (s_page == MONITOR_PAGE_TX) {
        if (event == UI_EVT_UP || event == UI_EVT_DOWN ||
            event == UI_EVT_LEFT || event == UI_EVT_RIGHT) {
            ui_bbox_t boxes[MONITOR_TX_ACTION_COUNT];
            build_tx_focus_boxes(boxes);
            ui_focus_move_spatial(UI_FOCUS_WIFI_MONITOR, boxes,
                                  MONITOR_TX_ACTION_COUNT, event);
        } else if (event == UI_EVT_SELECT && selected == 0) {
            /* Send beacon. Шлём только когда радио в PROMISCUOUS (окно инъекции).
             * Иначе движок всё равно отклонит в своей задаче, но пользователю
             * удобнее получить подсказку сразу. */
            if (wifi_worker_get_state() != WIFI_WORKER_STATE_PROMISCUOUS) {
                ui_popup_show_error("Start capture first");
            } else if (tx_engine_request_beacon(tx_config_ssid(),
                                                tx_config_burst_count()) != ESP_OK) {
                ui_popup_show_error("TX queue busy");
            }
        } else if (event == UI_EVT_SELECT && selected == 1) {
            open_ap_picker(PICK_DEAUTH); /* Send deauth → окно выбора AP */
        } else if (event == UI_EVT_SELECT && selected == 2) {
            open_ap_picker(PICK_PMKID);  /* Get PMKID → окно выбора AP */
        } else if (event == UI_EVT_SELECT && selected == 3) {
            start_page_transition(MONITOR_PAGE_SECONDARY); /* BACK */
        }
        /* selected == 4 (FRONT) — задел под следующую страницу, пока бездействует. */
        ui_render(canvas);
        return;
    }
    if ((event == UI_EVT_LEFT || event == UI_EVT_RIGHT) && selected == 0) {
        change_fixed_channel(event);
    } else if (event == UI_EVT_UP || event == UI_EVT_DOWN ||
               event == UI_EVT_LEFT || event == UI_EVT_RIGHT) {
        ui_bbox_t boxes[MONITOR_PRIMARY_ACTION_COUNT];
        build_primary_focus_boxes(boxes);
        ui_focus_move_spatial(UI_FOCUS_WIFI_MONITOR, boxes,
                              MONITOR_PRIMARY_ACTION_COUNT, event);
    } else if (event == UI_EVT_SELECT) {
        if (selected == 0) {
            if (!s_engine.running &&
                wifi_worker_get_state() != WIFI_WORKER_STATE_PROMISCUOUS) {
                s_start_hopper_after_capture = true;
                request_capture_start();
            } else {
                esp_err_t err = s_engine.running
                    ? wifi_worker_send_hopper_stop(&s_request_id)
                    : wifi_worker_send_hopper_start(DEFAULT_DWELL_MS,
                                                    &s_request_id);
                if (err != ESP_OK) show_queue_error();
                else s_phase = MONITOR_UPDATING_HOPPER;
            }
        } else if (selected == 1) {
            s_start_hopper_after_capture = false;
            if (wifi_worker_get_state() == WIFI_WORKER_STATE_PROMISCUOUS) {
                if (wifi_worker_send_disable_promiscuous(&s_request_id) != ESP_OK)
                    show_queue_error();
                else
                    s_phase = MONITOR_STOPPING_CAPTURE;
            } else {
                s_storage_status[0] = '\0';
                s_packets = s_management = s_data = s_control = s_eapol = 0;
                memset(s_graph, 0, sizeof(s_graph));
                s_graph_window_packets = 0;
                s_graph_last_tick = xTaskGetTickCount();
                request_capture_start();
            }
        } else if (selected == 2) {
            wifi_capture_save_status_t save = wifi_capture_store_save_request();
            if (save == WIFI_CAPTURE_SAVE_QUEUED)
                snprintf(s_storage_status, sizeof(s_storage_status), "Saving...");
            else if (save == WIFI_CAPTURE_SAVE_BUSY)
                ui_popup_show_error("Writer busy");
            else if (save == WIFI_CAPTURE_SAVE_EMPTY)
                ui_popup_show_error("Capture is empty");
            else
                ui_popup_show_error("Save failed");
        } else if (selected == 3) {
            request_monitor_exit();
        } else if (selected == 4) {
            start_page_transition(MONITOR_PAGE_SECONDARY);
        }
    }
    ui_render(canvas);
}

bool ui_wifi_monitor_handle_worker_result(const wifi_worker_result_t *result)
{
    if (result == NULL || result->request_id != s_request_id) return false;
    if (result->type == WIFI_WORKER_RESULT_STARTED &&
        s_phase == MONITOR_STARTING_RADIO) {
        request_capture_start();
    } else if (result->type == WIFI_WORKER_RESULT_DISCONNECTED &&
               s_phase == MONITOR_DISCONNECTING_STA) {
        request_capture_start();
    } else if (result->type == WIFI_WORKER_RESULT_PROMISCUOUS_ENABLED) {
        if (s_start_hopper_after_capture) {
            s_start_hopper_after_capture = false;
            if (wifi_worker_send_hopper_start(DEFAULT_DWELL_MS,
                                              &s_request_id) == ESP_OK)
                s_phase = MONITOR_UPDATING_HOPPER;
            else
                show_queue_error();
        } else {
            s_phase = MONITOR_IDLE;
        }
    } else if (result->type == WIFI_WORKER_RESULT_CHANNEL_SET) {
        s_manual_channel = result->channel;
        /* Парковка на канал цели завершена — теперь можно слать провокацию: радио
         * стоит там, где точка нас услышит и куда прилетит её M1 с PMKID. */
        if (s_pmkid_after_park) {
            s_pmkid_after_park = false;
            esp_err_t err = tx_engine_request_pmkid(
                s_pmkid_target.bssid, s_pmkid_target.ssid,
                s_pmkid_target.rsn_present ? &s_pmkid_target.rsn : NULL,
                MONITOR_PMKID_ROUNDS);
            if (err != ESP_OK) ui_popup_show_error("PMKID queue busy");
            /* успех — молча; результат виден по счётчику PMKID и файлу на SD */
        }
        s_phase = MONITOR_IDLE;
    } else if (result->type == WIFI_WORKER_RESULT_PROMISCUOUS_DISABLED) {
        s_phase = MONITOR_IDLE;
        if (s_exit_after_stop) leave_monitor();
    } else if (result->type == WIFI_WORKER_RESULT_HOPPER_STARTED) {
        s_phase = MONITOR_IDLE;
    } else if (result->type == WIFI_WORKER_RESULT_HOPPER_STOPPED) {
        uint8_t channel = wifi_channel_hopper_current_channel();
        if (channel != 0) s_manual_channel = channel;
        /* Хоппер остановлен ради парковки на канал цели PMKID — ставим канал;
         * solicit уйдёт после подтверждения канала (CHANNEL_SET). */
        if (s_pmkid_after_park) {
            uint8_t ch = pmkid_target_channel(&s_pmkid_target);
            if (ch < s_first_channel || ch > s_last_channel) ch = s_manual_channel;
            if (wifi_worker_send_set_channel(ch, &s_request_id) == ESP_OK) {
                s_manual_channel = ch;
                s_phase = MONITOR_PMKID_SET_CH;
            } else {
                s_pmkid_after_park = false;
                show_queue_error();
            }
        } else {
            s_phase = MONITOR_IDLE;
        }
    } else if (result->type == WIFI_WORKER_RESULT_ERROR) {
        s_phase = MONITOR_IDLE;
        s_start_hopper_after_capture = false;
        s_pmkid_after_park = false; /* цепочка парковки PMKID прервана ошибкой */
        ui_popup_show_error("Monitor failed");
    }
    return true;
}

bool ui_wifi_monitor_cursor_is_animating(void)
{
    return s_cursor.animating || s_transition.active;
}

void ui_wifi_monitor_poll(gfx_canvas_t *canvas)
{
    bool changed = false;
    if (s_phase == MONITOR_WAITING_FOR_IDLE && !connection_transition_active()) {
        s_phase = MONITOR_IDLE;
        request_capture_start();
        changed = true;
    }
    wifi_analyzer_snapshot_t analyzer;
    if (wifi_analyzer_get_snapshot(&analyzer)) {
        uint32_t packets = analyzer.packets > UINT32_MAX
            ? UINT32_MAX : (uint32_t)analyzer.packets;
        uint32_t management = analyzer.management_frames > UINT32_MAX
            ? UINT32_MAX : (uint32_t)analyzer.management_frames;
        uint32_t data = analyzer.data_frames > UINT32_MAX
            ? UINT32_MAX : (uint32_t)analyzer.data_frames;
        uint32_t control = analyzer.control_frames > UINT32_MAX
            ? UINT32_MAX : (uint32_t)analyzer.control_frames;
        uint32_t eapol = analyzer.eapol_frames > UINT32_MAX
            ? UINT32_MAX : (uint32_t)analyzer.eapol_frames;
        uint32_t delta = packets >= s_packets ? packets - s_packets : packets;
        if (delta || management != s_management || data != s_data ||
            control != s_control || eapol != s_eapol) changed = true;
        s_graph_window_packets += delta;
        s_packets = packets;
        s_management = management;
        s_data = data;
        s_control = control;
        s_eapol = eapol;
        s_analyzer_snapshot = analyzer;
        if (!s_analyzer_snapshot_valid) changed = true;
        s_analyzer_snapshot_valid = true;
    }
    wifi_ap_tracker_diagnostics_t ap_diagnostics;
    if (wifi_ap_tracker_get_diagnostics(&ap_diagnostics)) {
        if (!s_ap_snapshot_valid ||
            memcmp(&ap_diagnostics, &s_ap_diagnostics,
                   sizeof(ap_diagnostics)) != 0) changed = true;
        s_ap_diagnostics = ap_diagnostics;
        s_ap_snapshot_valid = true;
    }
    wifi_sta_tracker_diagnostics_t sta_diagnostics;
    if (wifi_sta_tracker_get_diagnostics(&sta_diagnostics)) {
        if (!s_sta_snapshot_valid ||
            memcmp(&sta_diagnostics, &s_sta_diagnostics,
                   sizeof(sta_diagnostics)) != 0) changed = true;
        s_sta_diagnostics = sta_diagnostics;
        s_sta_snapshot_valid = true;
    }
    uint16_t completed_handshakes;
    if (wifi_eapol_tracker_get_completed_count(&completed_handshakes)) {
        if (!s_handshake_snapshot_valid ||
            completed_handshakes != s_completed_handshakes) changed = true;
        s_completed_handshakes = completed_handshakes;
        s_handshake_snapshot_valid = true;
    }
    /* Кешируем число пойманных PMKID так же, как остальную статистику: сравниваем
     * с прошлым значением и просим перерисовку только при изменении. */
    uint16_t pmkid_count = wifi_pmkid_store_count();
    if (pmkid_count != s_pmkid_count) {
        s_pmkid_count = pmkid_count;
        changed = true;
    }
    /* Статус автосохранения PMKID на SD: при ошибке (нет карты и т.п.) подсветим
     * счётчик красным — «ловим, но не сохраняем». */
    esp_err_t pmkid_save;
    if (wifi_pmkid_writer_take_result(&pmkid_save)) {
        bool failed = (pmkid_save != ESP_OK);
        if (failed != s_pmkid_sd_error) {
            s_pmkid_sd_error = failed;
            changed = true;
        }
    }
    wifi_pipeline_diagnostics_t diagnostics;
    if (wifi_analyzer_get_diagnostics(&diagnostics) &&
        memcmp(&diagnostics, &s_diagnostics, sizeof(diagnostics)) != 0) {
        s_diagnostics = diagnostics;
        changed = true;
    }
    wifi_channel_engine_snapshot_t engine;
    if (wifi_channel_engine_get_snapshot(&engine) &&
        memcmp(&engine, &s_engine, sizeof(engine)) != 0) {
        s_engine = engine;
        s_first_channel = engine.first_channel;
        s_last_channel = engine.last_channel;
        changed = true;
    }
    wifi_channel_observation_t channels[WIFI_ANALYZER_MAX_CHANNELS];
    uint8_t first_channel, last_channel;
    if (wifi_analyzer_get_channel_observations(
            channels, &first_channel, &last_channel) &&
        memcmp(channels, s_channels, sizeof(channels)) != 0) {
        memcpy(s_channels, channels, sizeof(channels));
        changed = true;
    }
    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed = (uint32_t)(TickType_t)(now - s_graph_last_tick) *
                       1000U / configTICK_RATE_HZ;
    if (elapsed >= GRAPH_SAMPLE_MS) {
        memmove(s_graph, s_graph + 1,
                (GRAPH_SAMPLES - 1U) * sizeof(s_graph[0]));
        s_graph[GRAPH_SAMPLES - 1U] = s_graph_window_packets > UINT16_MAX
            ? UINT16_MAX : (uint16_t)s_graph_window_packets;
        s_graph_window_packets = 0;
        s_graph_last_tick = now;
        changed = true;
    }
    esp_err_t save_result;
    char saved_path[96];
    if (wifi_capture_store_take_save_result(
            &save_result, saved_path, sizeof(saved_path))) {
        if (save_result == ESP_OK) {
            const char *name = strrchr(saved_path, '/');
            snprintf(s_storage_status, sizeof(s_storage_status), "Saved: %.31s",
                     name != NULL ? name + 1 : saved_path);
        } else if (save_result == ESP_ERR_NOT_FOUND ||
                   save_result == ESP_ERR_INVALID_STATE) {
            snprintf(s_storage_status, sizeof(s_storage_status), "SD unavailable");
        } else if (save_result == ESP_ERR_NO_MEM) {
            snprintf(s_storage_status, sizeof(s_storage_status), "Storage full");
        } else {
            snprintf(s_storage_status, sizeof(s_storage_status), "Save I/O error");
        }
        changed = true;
    }
    if (changed && canvas != NULL && ui_screen_get() == UI_SCREEN_WIFI_MONITOR)
        ui_render(canvas);
}

static void draw_navigation(gfx_canvas_t *canvas, int16_t x_offset,
                            uint8_t back_index, uint8_t front_index,
                            bool draw_cursor)
{
    static const char back[] = "< BACK";
    static const char front[] = "FRONT >";
    const int16_t y = 164;
    const int16_t back_x = 10 + x_offset;
    const int16_t front_width = gfx_canvas_measure_text_width(UI_FONT, front);
    const int16_t front_x = DISPLAY_WIDTH - 10 - front_width + x_offset;
    uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_MONITOR);

    gfx_canvas_draw_str(canvas, back_x, y, back, UI_FONT,
                        selected == back_index ? GFX_WHITE : GFX_DARKGRAY);
    gfx_canvas_draw_str(canvas, front_x, y, front, UI_FONT,
                        selected == front_index ? GFX_WHITE : GFX_DARKGRAY);

    if (draw_cursor) {
        const char *text = selected == back_index ? back : front;
        int16_t x = selected == back_index ? back_x : front_x;
        int16_t width = gfx_canvas_measure_text_width(UI_FONT, text);
        ui_cursor_set_target(&s_cursor, x - 3, y - 13, width + 6, 17);
        ui_cursor_step(&s_cursor);
        ui_cursor_draw(canvas, &s_cursor, GFX_WHITE);
    }
}

static void draw_primary_page(gfx_canvas_t *canvas, int16_t x_offset,
                              bool draw_cursor)
{
    gfx_canvas_draw_str(canvas, 10 + x_offset, 12, "Wi-Fi Air Monitor",
                        UI_FONT, GFX_WHITE);
    gfx_canvas_draw_line(canvas, x_offset, 18,
                         x_offset + DISPLAY_WIDTH - 1, 18, GFX_WHITE);

    bool capture = wifi_worker_get_state() == WIFI_WORKER_STATE_PROMISCUOUS;
    uint8_t channel = s_engine.current_channel != 0
        ? s_engine.current_channel : s_manual_channel;
    char line[64];
    snprintf(line, sizeof(line), "CH %02u  %s  %s", channel,
             s_engine.running ? "SURVEY" : "FIXED",
             capture ? "RUN" : "STOP");
    gfx_canvas_draw_str(canvas, 10 + x_offset, 35, line, UI_FONT,
                        capture ? GFX_GREEN : GFX_DARKGRAY);

    const int16_t gx = 10 + x_offset, gy = 43, gw = 205, gh = 45;
    gfx_canvas_draw_rect(canvas, gx, gy, gw, gh, GFX_DARKGRAY);
    uint16_t peak = 1;
    for (uint8_t i = 0; i < GRAPH_SAMPLES; ++i)
        if (s_graph[i] > peak) peak = s_graph[i];
    for (uint8_t i = 0; i < GRAPH_SAMPLES; ++i) {
        int16_t x = gx + 3 + (int16_t)i * 7;
        int16_t h = (int32_t)s_graph[i] * (gh - 5) / peak;
        if (h > 0)
            gfx_canvas_draw_line(canvas, x, gy + gh - 3,
                                 x, gy + gh - 3 - h, GFX_CYAN);
    }

    format_count(line, sizeof(line), s_diagnostics.rx_packets_per_second);
    gfx_canvas_draw_str(canvas, 228 + x_offset, 52, line, UI_FONT, GFX_WHITE);
    gfx_canvas_draw_str(canvas, 228 + x_offset, 66, "PKT/S", UI_FONT, GFX_DARKGRAY);
    char eapol_count[12], mgmt_count[12], data_count[12];
    format_count(eapol_count, sizeof(eapol_count), s_eapol);
    format_count(mgmt_count, sizeof(mgmt_count), s_management);
    format_count(data_count, sizeof(data_count), s_data);
    snprintf(line, sizeof(line), "E:%s", eapol_count);
    gfx_canvas_draw_str(canvas, 228 + x_offset, 83, line, UI_FONT, GFX_YELLOW);

    uint32_t dropped = s_diagnostics.ingress_dropped;
    snprintf(line, sizeof(line), "DROP:%lu", (unsigned long)dropped);
    gfx_canvas_draw_str(canvas, 10 + x_offset, 104, line, UI_FONT,
                        dropped == 0 ? GFX_GREEN : GFX_RED);
    snprintf(line, sizeof(line), "M:%s", mgmt_count);
    gfx_canvas_draw_str(canvas, 126 + x_offset, 104, line, UI_FONT, GFX_DARKGRAY);
    snprintf(line, sizeof(line), "D:%s", data_count);
    gfx_canvas_draw_str(canvas, 224 + x_offset, 104, line, UI_FONT, GFX_DARKGRAY);

    char channel_action[32];
    snprintf(channel_action, sizeof(channel_action), "Channel: %s %02u",
             s_engine.running ? "SURVEY" : "FIXED", channel);
    const char *actions[] = {
        channel_action,
        capture ? "Stop capture" : "Start capture",
        wifi_capture_store_is_saving() ? "Saving PCAP..." :
            (s_storage_status[0] != '\0' ? s_storage_status : "Save PCAP"),
    };
    uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_MONITOR);
    for (uint8_t i = 0; i < 3U; ++i) {
        int16_t x = 10 + x_offset;
        int16_t y = 116 + (int16_t)i * 16;
        gfx_canvas_draw_str(canvas, x, y, actions[i], UI_FONT,
                            i == selected ? GFX_WHITE : GFX_DARKGRAY);
        if (draw_cursor && i == selected) {
            int16_t width = gfx_canvas_measure_text_width(UI_FONT, actions[i]);
            if (width > DISPLAY_WIDTH - 17) width = DISPLAY_WIDTH - 17;
            ui_cursor_set_target(&s_cursor, x - 3, y - 13, width + 6, 17);
            ui_cursor_step(&s_cursor);
            ui_cursor_draw(canvas, &s_cursor, GFX_WHITE);
        }
    }
    draw_navigation(canvas, x_offset, 3U, 4U, draw_cursor && selected >= 3U);
}

static void draw_secondary_stat(gfx_canvas_t *canvas, int16_t x,
                                int16_t label_y, const char *label,
                                bool valid, uint64_t value, uint16_t color)
{
    char number[24];
    gfx_canvas_draw_str(canvas, x, label_y, label, UI_FONT, GFX_DARKGRAY);
    if (valid)
        snprintf(number, sizeof(number), "%" PRIu64, value);
    else
        snprintf(number, sizeof(number), "--");
    gfx_canvas_draw_str(canvas, x, label_y + 12, number, UI_FONT, color);
}

static void draw_secondary_page(gfx_canvas_t *canvas, int16_t x_offset,
                                bool draw_cursor)
{
    gfx_canvas_draw_str(canvas, 10 + x_offset, 12, "Wi-Fi Air Monitor",
                        UI_FONT, GFX_WHITE);
    gfx_canvas_draw_line(canvas, x_offset, 18,
                         x_offset + DISPLAY_WIDTH - 1, 18, GFX_WHITE);

    const int16_t left_x = 10 + x_offset;
    const int16_t right_x = 166 + x_offset;
    draw_secondary_stat(canvas, left_x, 32, "APs:", s_ap_snapshot_valid,
                        s_ap_diagnostics.table_count, GFX_WHITE);
    draw_secondary_stat(canvas, right_x, 32, "Stations:",
                        s_sta_snapshot_valid, s_sta_diagnostics.table_count,
                        GFX_WHITE);
    draw_secondary_stat(canvas, left_x, 60, "Mgmt:",
                        s_analyzer_snapshot_valid,
                        s_analyzer_snapshot.management_frames, GFX_WHITE);
    draw_secondary_stat(canvas, right_x, 60, "Data:",
                        s_analyzer_snapshot_valid,
                        s_analyzer_snapshot.data_frames, GFX_WHITE);
    draw_secondary_stat(canvas, left_x, 88, "Ctrl:",
                        s_analyzer_snapshot_valid,
                        s_analyzer_snapshot.control_frames, GFX_WHITE);
    draw_secondary_stat(canvas, right_x, 88, "EAPOL:",
                        s_analyzer_snapshot_valid,
                        s_analyzer_snapshot.eapol_frames, GFX_YELLOW);
    /* Короткая метка «HS:» вместо «Handshakes:»: при xAdvance=16 длинная надпись
     * (~176 px от x=10) налезала на «PMKID:» в правой колонке (x=166). */
    draw_secondary_stat(canvas, left_x, 116, "HS:",
                        s_handshake_snapshot_valid, s_completed_handshakes,
                        GFX_WHITE);
    /* PMKID стоит парой к Handshakes (обе — «добыча» для офлайн-подбора). Рисуем
     * из кеша s_pmkid_count (обновляется в poll, как и соседние счётчики). Жёлтый —
     * как EAPOL; красный — если автозапись на SD не удалась (ловим, но не сохраняем).
     * valid=true: 0 — тоже валидное число. */
    draw_secondary_stat(canvas, right_x, 116, "PMKID:",
                        true, s_pmkid_count,
                        s_pmkid_sd_error ? GFX_RED : GFX_YELLOW);

    draw_navigation(canvas, x_offset, 0U, 1U, draw_cursor);
}

static void draw_tx_page(gfx_canvas_t *canvas, int16_t x_offset, bool draw_cursor)
{
    /* Шапка — тот же паттерн, что у других страниц: заголовок + линия на y=18. */
    gfx_canvas_draw_str(canvas, 10 + x_offset, 12, "Wi-Fi TX", UI_FONT, GFX_WHITE);
    gfx_canvas_draw_line(canvas, x_offset, 18,
                         x_offset + DISPLAY_WIDTH - 1, 18, GFX_WHITE);

    /* Состояние радио: отправка возможна только в PROMISCUOUS (окно инъекции). */
    bool capture = wifi_worker_get_state() == WIFI_WORKER_STATE_PROMISCUOUS;
    gfx_canvas_draw_str(canvas, 10 + x_offset, 35,
                        capture ? "Radio: RUN" : "Radio: STOP - start capture",
                        UI_FONT, capture ? GFX_GREEN : GFX_DARKGRAY);

    /* Единственная статистика — счётчик реально отправленных кадров (TX). */
    char line[32];
    snprintf(line, sizeof(line), "Sent: %lu",
             (unsigned long)tx_engine_sent_count());
    gfx_canvas_draw_str(canvas, 10 + x_offset, 60, line, UI_FONT, GFX_WHITE);

    /* Кнопки-функции: по одной в строке, как на остальных страницах.
     * Индексы фокуса: 0 = Send beacon, 1 = Send deauth, 2 = Get PMKID. */
    static const char *const actions[] = { "Send beacon", "Send deauth", "Get PMKID" };
    uint8_t selected = ui_focus_get(UI_FOCUS_WIFI_MONITOR);
    for (uint8_t i = 0; i < 3U; ++i) {
        const int16_t x = 10 + x_offset;
        const int16_t y = 116 + (int16_t)i * 16;
        gfx_canvas_draw_str(canvas, x, y, actions[i], UI_FONT,
                            i == selected ? GFX_WHITE : GFX_DARKGRAY);
        if (draw_cursor && i == selected) {
            int16_t width = gfx_canvas_measure_text_width(UI_FONT, actions[i]);
            ui_cursor_set_target(&s_cursor, x - 3, y - 13, width + 6, 17);
            ui_cursor_step(&s_cursor);
            ui_cursor_draw(canvas, &s_cursor, GFX_WHITE);
        }
    }

    /* Футер BACK(3)/FRONT(4). Курсор футера — когда фокус на нём (индекс >= 3);
     * иначе курсор уже нарисован на одной из кнопок выше. */
    draw_navigation(canvas, x_offset, 3U, 4U, draw_cursor && selected >= 3U);
}

/* Модальное окно выбора AP для deauth — выглядит как список сетей (как Networks):
 * прокручиваемый перечень увиденных точек, по строке на точку. Рисуется поверх
 * всего, фон уже залит чёрным в ui_wifi_monitor_draw. */
static void draw_ap_picker(gfx_canvas_t *canvas)
{
    bool pmkid = (s_pick_action == PICK_PMKID);
    gfx_canvas_draw_str(canvas, 10, 12,
                        pmkid ? "PMKID target" : "Deauth target",
                        UI_FONT, GFX_WHITE);
    gfx_canvas_draw_line(canvas, 0, 18, DISPLAY_WIDTH - 1, 18, GFX_WHITE);

    for (uint8_t row = 0; row < DEAUTH_PICKER_VISIBLE_ROWS; ++row) {
        uint16_t i = (uint16_t)(s_pick_top + row);
        if (i >= s_pick_aps.count) break;
        const wifi_tracked_ap_t *ap = &s_pick_aps.records[i];
        const int16_t x = 10;
        const int16_t y = 34 + (int16_t)row * 16;
        const bool sel = (i == s_pick_sel);

        /* SSID (или «(hidden)»), RSSI и канал — как на экране сетей. */
        char text[48];
        const char *name = ap->ssid_length > 0U ? ap->ssid : "(hidden)";
        snprintf(text, sizeof(text), "%.18s  %ddBm  c%u", name,
                 (int)ap->rssi_last, (unsigned)ap->rx_channel);
        gfx_canvas_draw_str(canvas, x, y, text, UI_FONT,
                            sel ? GFX_WHITE : GFX_DARKGRAY);
        if (sel) {
            int16_t width = gfx_canvas_measure_text_width(UI_FONT, text);
            if (width > DISPLAY_WIDTH - 17) width = DISPLAY_WIDTH - 17;
            ui_cursor_set_target(&s_cursor, x - 3, y - 13, width + 6, 17);
            ui_cursor_step(&s_cursor);
            ui_cursor_draw(canvas, &s_cursor, GFX_WHITE);
        }
    }

    gfx_canvas_draw_str(canvas, 10, 164,
                        pmkid ? "SEL: get PMKID   LEFT: cancel"
                              : "SEL: deauth   LEFT: cancel",
                        UI_FONT, GFX_DARKGRAY);
}

/* Диспетчер: нарисовать нужную страницу со сдвигом x_offset. Один вход для всей
 * логики слайда — чтобы отрисовка перехода не знала про конкретные страницы. */
static void draw_page(gfx_canvas_t *canvas, monitor_page_t page,
                      int16_t x_offset, bool draw_cursor)
{
    switch (page) {
    case MONITOR_PAGE_PRIMARY:
        draw_primary_page(canvas, x_offset, draw_cursor);
        break;
    case MONITOR_PAGE_SECONDARY:
        draw_secondary_page(canvas, x_offset, draw_cursor);
        break;
    case MONITOR_PAGE_TX:
        draw_tx_page(canvas, x_offset, draw_cursor);
        break;
    }
}

void ui_wifi_monitor_draw(gfx_canvas_t *canvas)
{
    gfx_canvas_fill(canvas, GFX_BLACK);

    /* Модальное окно выбора цели (deauth/PMKID) рисуется поверх страниц. */
    if (s_pick_action != PICK_NONE) {
        draw_ap_picker(canvas);
        return;
    }

    /* Нет перехода — просто рисуем текущую страницу. */
    if (!s_transition.active) {
        draw_page(canvas, s_page, 0, true);
        return;
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = (uint32_t)(TickType_t)(now - s_transition.started_at) *
                          1000U / configTICK_RATE_HZ;

    /* Переход закончился — фиксируем целевую страницу и рисуем её на месте. */
    if (elapsed_ms >= MONITOR_SLIDE_MS) {
        s_page = s_transition.to;
        s_transition.active = false;
        ui_cursor_reset(&s_cursor);
        draw_page(canvas, s_page, 0, true);
        return;
    }

    /* Середина слайда. shift растёт 0..DISPLAY_WIDTH. Направление берём из
     * порядка страниц: to > from — движемся «вперёд» (новая страница въезжает
     * справа, старая уезжает влево); иначе — «назад» (зеркально). Геометрия
     * офсетов идентична прежней для PRIMARY<->SECONDARY. */
    int16_t shift = (int32_t)DISPLAY_WIDTH * elapsed_ms / MONITOR_SLIDE_MS;
    bool forward = s_transition.to > s_transition.from;
    if (forward) {
        draw_page(canvas, s_transition.from, -shift, false);
        draw_page(canvas, s_transition.to, DISPLAY_WIDTH - shift, true);
    } else {
        draw_page(canvas, s_transition.from, shift, false);
        draw_page(canvas, s_transition.to, shift - DISPLAY_WIDTH, true);
    }
}
