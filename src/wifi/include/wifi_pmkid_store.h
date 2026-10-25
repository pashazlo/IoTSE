#pragma once

/* =====================================================================
 *  WIFI PMKID STORE — ограниченное хранилище пойманных PMKID с дедупликацией
 * =====================================================================
 *
 * ЗАЧЕМ. Экстрактор (wifi_pmkid_parser) достаёт PMKID из каждого M1. Но один и
 * тот же AP шлёт M1 при каждом переподключении/ретрансмите, поэтому «сырые»
 * находки повторяются. Этот слой хранит только УНИКАЛЬНЫЕ результаты (ключ —
 * BSSID+STA+PMKID), чтобы счётчик не врал и автосейв не плодил дубликаты.
 *
 * АРХИТЕКТУРА. Пишет сюда задача анализатора (observe), читает UI (count/snapshot) —
 * две задачи, поэтому доступ к общему экземпляру идёт под коротким mutex. ЧИСТАЯ
 * логика вынесена в core (wifi_pmkid_store_internal.h + wifi_pmkid_store_core.c):
 * она работает над переданным состоянием, без статиков и RTOS, и проверяется
 * host-тестом. Эта публичная обёртка добавляет единственный экземпляр и mutex.
 *
 * ПАМЯТЬ. Ограниченный массив записей в RAM, без per-frame malloc. Snapshot —
 * копия для UI, чтобы рисовать стабильные данные без удержания mutex.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef WIFI_PMKID_STORE_HOST_TEST
typedef int esp_err_t; /* на host ESP-IDF нет — хватает заглушки для core-тестов */
#else
#include "esp_err.h"
#endif

#include "wifi_pmkid_parser.h" /* WIFI_PMKID_LEN */

#ifdef __cplusplus
extern "C" {
#endif

/* Сколько уникальных PMKID держим одновременно. Каждая запись мала, 32 с запасом
 * покрывает лабораторную сеть; переполнение учитываем в dropped_full. */
#define WIFI_PMKID_STORE_CAPACITY 32U

/* Длина SSID (без завершающего NUL), как в остальном Wi-Fi-коде. */
#define WIFI_PMKID_SSID_MAX 32U

/* Одна «кристальная» запись: всё, что нужно для строки hashcat 22000
 * (WPA*01*pmkid*ap_mac*sta_mac*essid) плюс RSSI/время для показа. */
typedef struct {
    uint8_t pmkid[WIFI_PMKID_LEN];        /* 16 байт PMKID */
    uint8_t bssid[6];                     /* MAC точки (AA) */
    uint8_t sta[6];                       /* MAC клиента (SPA) */
    char    ssid[WIFI_PMKID_SSID_MAX + 1];/* имя сети + NUL (может быть пустым) */
    int8_t  rssi;                         /* RSSI кадра M1 */
    int64_t ts_us;                        /* время наблюдения, мкс */
} wifi_pmkid_record_t;

/* Снимок для UI: счётчики + копия записей. Велик (~1.8 КБ) — НЕ класть на стек
 * мелкой задачи; использовать статику/PSRAM/кучу. */
typedef struct {
    uint16_t count;                       /* сколько уникальных записей сейчас */
    uint32_t total_seen;                  /* все наблюдения (вкл. дубли и переполнение) */
    uint32_t dropped_full;                /* новые уникальные, не влезшие из-за лимита */
    wifi_pmkid_record_t records[WIFI_PMKID_STORE_CAPACITY];
} wifi_pmkid_store_snapshot_t;

/* --- Публичное API (обёртка с mutex; реализация — отдельным шагом) --- */

/* Создать хранилище и mutex. Идемпотентно. */
esp_err_t wifi_pmkid_store_init(void);

/* Очистить (например, при старте новой сессии захвата). */
void wifi_pmkid_store_reset(void);

/* Учесть находку. Возвращает true, если запись НОВАЯ уникальная (её стоит дописать
 * в файл). Дубликат или переполнение → false. */
bool wifi_pmkid_store_observe(const wifi_pmkid_record_t *rec);

/* Текущее число уникальных записей (для счётчика на экране). */
uint16_t wifi_pmkid_store_count(void);

/* Снять копию состояния для отрисовки. true — скопировано. */
bool wifi_pmkid_store_get_snapshot(wifi_pmkid_store_snapshot_t *out);

#ifdef __cplusplus
}
#endif
