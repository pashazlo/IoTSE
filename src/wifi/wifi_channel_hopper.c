#include "wifi_channel_hopper.h"
#include "wifi_channel_hopper_internal.h"
#include "wifi_worker.h"
#include "wifi_promiscuous.h" /* Фаза 5: нужен rx_total как сигнал активности канала. */
#include <stdatomic.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

/* ===== Фаза 5: взвешенный (адаптивный) dwell =====================
 * Раньше в SURVEY-режиме мы стояли на каждом канале ровно s_interval мс, включая
 * мёртвые каналы — и теряли там время, которое могли бы ловить кадры на активных.
 *
 * Идея: когда интервал на канале истёк, смотрим, сколько кадров пришло за этот
 * "слайс" (rx_total сейчас минус rx_total на начало слайса). Если канал активен —
 * выдаём ему ещё один слайс (остаёмся), и так до лимита продлений. Пустой канал
 * проскакиваем сразу, как и раньше. Это чистое перераспределение времени в пользу
 * каналов с трафиком — больше пойманных кадров без потерь в конвейере.
 *
 * Анти-starvation: продлений не больше WIFI_HOPPER_MAX_EXTENSIONS, поэтому даже
 * очень загруженный канал рано или поздно отпускает обход, и остальные каналы не
 * голодают. Максимальное время на одном канале = (1 + MAX_EXTENSIONS) * s_interval.
 *
 * Затрагивает ТОЛЬКО SURVEY: в FIXED hopper не запущен и tick выходит сразу. */
#define WIFI_HOPPER_ACTIVE_FRAMES   8U /* кадров за слайс, чтобы считать канал активным */
#define WIFI_HOPPER_MAX_EXTENSIONS  3U /* макс. число доп. слайсов на один канал */

static atomic_bool s_running;
static atomic_int s_error;
/* These variables belong only to the worker task. */
static TickType_t s_interval, s_last_switch;
static uint8_t s_channel;
static atomic_uchar s_published_channel;
static atomic_int s_mode;
static portMUX_TYPE s_dwell_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_dwell_us[14];
static int64_t s_channel_started_us;
static uint8_t s_first_channel, s_last_channel;
static uint32_t s_dwell_ms = 250;
/* Фаза 5, только воркер-таск (как и s_interval/s_last_switch/s_channel):
 * s_rx_at_slice — значение rx_total на начало текущего слайса dwell;
 * s_extensions  — сколько доп. слайсов уже выдано текущему каналу. */
static uint64_t s_rx_at_slice;
static uint8_t s_extensions;

/* Фаза 5: неблокирующее чтение счётчика принятых кадров. rx_total монотонно
 * растёт в течение сессии (сбрасывается только при init/reset промискуитета до
 * старта хоппинга), поэтому rx_now - s_rx_at_slice не уходит в underflow. На
 * неудачу (пул ещё не готов) возвращаем false — вызывающий тогда просто
 * переключит канал как раньше. */
static bool hopper_rx_total(uint64_t *out)
{
    wifi_promiscuous_snapshot_t snapshot;
    if (!wifi_promiscuous_get_snapshot(&snapshot)) return false;
    *out = snapshot.rx_total;
    return true;
}

static void account_dwell(int64_t now)
{
    portENTER_CRITICAL(&s_dwell_lock);
    if (s_channel >= 1U && s_channel <= 14U && s_channel_started_us != 0) {
        s_dwell_us[s_channel - 1U] += (uint64_t)(now - s_channel_started_us);
    }
    s_channel_started_us = now;
    portEXIT_CRITICAL(&s_dwell_lock);
}

esp_err_t wifi_channel_hopper_start(uint32_t interval_ms)
{
    uint32_t id;
    return wifi_worker_send_hopper_start(interval_ms, &id);
}

esp_err_t wifi_channel_hopper_stop(void)
{
    uint32_t id;
    return wifi_worker_send_hopper_stop(&id);
}

bool wifi_channel_hopper_is_running(void)
{
    return atomic_load_explicit(&s_running, memory_order_acquire);
}

esp_err_t wifi_channel_hopper_last_error(void)
{
    return atomic_load_explicit(&s_error, memory_order_acquire);
}

uint8_t wifi_channel_hopper_current_channel(void)
{
    return atomic_load_explicit(&s_published_channel, memory_order_acquire);
}

void wifi_channel_hopper_worker_stop(void)
{
    if (wifi_radio_is_promiscuous_enabled()) {
        account_dwell(esp_timer_get_time());
    }
    atomic_store_explicit(&s_running, false, memory_order_release);
    atomic_store_explicit(&s_mode, WIFI_CHANNEL_MODE_FIXED, memory_order_release);
}

void wifi_channel_engine_worker_begin_fixed(uint8_t channel)
{
    uint8_t first = 0, last = 0;
    (void)wifi_radio_get_channel_range(&first, &last);
    portENTER_CRITICAL(&s_dwell_lock);
    memset(s_dwell_us, 0, sizeof(s_dwell_us));
    s_first_channel = first;
    s_last_channel = last;
    s_channel = channel;
    s_channel_started_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_dwell_lock);
    atomic_store_explicit(&s_published_channel, channel, memory_order_release);
    atomic_store_explicit(&s_running, false, memory_order_release);
    atomic_store_explicit(&s_mode, WIFI_CHANNEL_MODE_FIXED, memory_order_release);
}

void wifi_channel_engine_worker_set_fixed(uint8_t channel)
{
    wifi_channel_hopper_worker_stop();
    portENTER_CRITICAL(&s_dwell_lock);
    s_channel = channel;
    s_channel_started_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_dwell_lock);
    atomic_store_explicit(&s_published_channel, channel, memory_order_release);
    atomic_store_explicit(&s_mode, WIFI_CHANNEL_MODE_FIXED, memory_order_release);
}

static esp_err_t fail(esp_err_t err)
{
    atomic_store_explicit(&s_error, err, memory_order_release);
    wifi_channel_hopper_worker_stop();
    ESP_LOGW("WIFI_HOPPER", "Hopping stopped: %s", esp_err_to_name(err));
    return err;
}

esp_err_t wifi_channel_hopper_worker_start(uint32_t interval_ms)
{
    if (interval_ms == 0) interval_ms = 250;
    if (interval_ms < 50 || interval_ms > 60000) return ESP_ERR_INVALID_ARG;
    if (!wifi_radio_is_promiscuous_enabled()) return ESP_ERR_INVALID_STATE;
    uint8_t first, last;
    esp_err_t err = wifi_radio_get_channel_range(&first, &last);
    if (err != ESP_OK) return fail(err);
    account_dwell(esp_timer_get_time());
    err = wifi_radio_promiscuous_set_channel(first);
    if (err != ESP_OK) return fail(err);
    portENTER_CRITICAL(&s_dwell_lock);
    s_channel = first;
    s_first_channel = first;
    s_last_channel = last;
    s_dwell_ms = interval_ms;
    s_channel_started_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_dwell_lock);
    atomic_store_explicit(&s_published_channel, first, memory_order_release);
    s_interval = pdMS_TO_TICKS(interval_ms);
    if (s_interval == 0) s_interval = 1;
    s_last_switch = xTaskGetTickCount();
    /* Фаза 5: начинаем слайс на первом канале с чистым счётчиком продлений. */
    s_extensions = 0;
    if (!hopper_rx_total(&s_rx_at_slice)) s_rx_at_slice = 0;
    atomic_store_explicit(&s_error, ESP_OK, memory_order_release);
    atomic_store_explicit(&s_running, true, memory_order_release);
    atomic_store_explicit(&s_mode, WIFI_CHANNEL_MODE_SURVEY, memory_order_release);
    return ESP_OK;
}

void wifi_channel_hopper_worker_tick(void)
{
    if (!wifi_channel_hopper_is_running()) return;
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - s_last_switch) < s_interval) return;

    /* Фаза 5: интервал истёк — решаем остаться или переключиться.
     * Сколько кадров пришло за этот слайс = rx_total сейчас - rx_total на старте
     * слайса. Если канал активен и лимит продлений не исчерпан — остаёмся ещё на
     * один слайс (канал НЕ меняем, поэтому account_dwell не вызываем: фактическое
     * время добавится при реальном переключении из s_channel_started_us). */
    uint64_t rx_now;
    if (hopper_rx_total(&rx_now) &&
        rx_now - s_rx_at_slice >= WIFI_HOPPER_ACTIVE_FRAMES &&
        s_extensions < WIFI_HOPPER_MAX_EXTENSIONS) {
        ++s_extensions;
        s_rx_at_slice = rx_now;
        s_last_switch = now;
        return;
    }

    /* Канал пустой или исчерпан лимит продлений — переключаемся на следующий. */
    uint8_t first, last;
    esp_err_t err = wifi_radio_get_channel_range(&first, &last);
    if (err != ESP_OK) { (void)fail(err); return; }
    uint8_t next = s_channel < first || s_channel >= last ? first : s_channel + 1;
    err = wifi_radio_promiscuous_set_channel(next);
    if (err != ESP_OK) { (void)fail(err); return; }
    account_dwell(esp_timer_get_time());
    portENTER_CRITICAL(&s_dwell_lock);
    s_channel = next;
    portEXIT_CRITICAL(&s_dwell_lock);
    atomic_store_explicit(&s_published_channel, next, memory_order_release);
    s_last_switch = now; /* No burst of catch-up channel switches. */
    /* Фаза 5: новый канал — обнуляем продления и базу отсчёта трафика слайса. */
    s_extensions = 0;
    if (!hopper_rx_total(&s_rx_at_slice)) s_rx_at_slice = 0;
}

wifi_channel_mode_t wifi_channel_engine_mode(void)
{
    return (wifi_channel_mode_t)atomic_load_explicit(&s_mode, memory_order_acquire);
}

bool wifi_channel_engine_get_snapshot(wifi_channel_engine_snapshot_t *out)
{
    if (out == NULL) return false;
    *out = (wifi_channel_engine_snapshot_t) {
        .mode = wifi_channel_engine_mode(),
        .current_channel = wifi_channel_hopper_current_channel(),
        .running = wifi_channel_hopper_is_running(),
    };
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_dwell_lock);
    out->first_channel = s_first_channel;
    out->last_channel = s_last_channel;
    out->dwell_ms = s_dwell_ms;
    memcpy(out->dwell_time_us, s_dwell_us, sizeof(s_dwell_us));
    if (wifi_radio_is_promiscuous_enabled() &&
        out->current_channel >= 1U && out->current_channel <= 14U &&
        s_channel_started_us != 0) {
        out->dwell_time_us[out->current_channel - 1U] +=
            (uint64_t)(now - s_channel_started_us);
    }
    portEXIT_CRITICAL(&s_dwell_lock);
    return true;
}
