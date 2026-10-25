#pragma once

/* =====================================================================
 *  WIFI PMKID PARSER — извлечение PMKID из EAPOL-Key (сообщение M1)
 * =====================================================================
 *
 * ЧТО ЭТО. PMKID (Pairwise Master Key IDentifier) — 16 байт, которые точка
 * доступа МОЖЕТ положить в первый кадр 4-way handshake (M1) внутри поля Key Data
 * как RSN-KDE особого вида. Этого одного кадра достаточно для офлайн-подбора
 * пароля (hashcat 22000), без полного хендшейка и без реального клиента.
 *
 * ГДЕ В АРХИТЕКТУРЕ. Это самый нижний слой фичи PMKID — ЧИСТАЯ ФУНКЦИЯ без
 * состояния, памяти и зависимостей от радио/RTOS. Она только разбирает байты.
 * Такой слой легко проверить host-тестом и трудно сломать. Вызывать её будет
 * тракт захвата (wifi_analyzer) на кадрах, классифицированных как M1.
 *
 * ФОРМАТ KDE PMKID (RSN). Key Data состоит из KDE — элементов вида
 *   [0xDD][len][OUI 3][type 1][data...]
 * Нужный нам PMKID-KDE: OUI = 00-0F-AC, type = 0x04, data = PMKID[16],
 * то есть len == 4 + 16 == 20 (0x14). «Кристальность» = СТРОГОЕ совпадение
 * тега, OUI, типа и длины; всё остальное игнорируем.
 *
 * ДОПУЩЕНИЕ ПРО MIC. Как и существующий wifi_eapol_parser, считаем Key MIC
 * длиной 16 байт (классический WPA2-PSK descriptor), поэтому поле key_data_length
 * лежит на фиксированном смещении. Для целей PMKID-атаки (WPA2-PSK) это верно.
 *
 * ЗАКОННОСТЬ. Только своё оборудование и разрешённое тестирование.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Длина PMKID в байтах. */
#define WIFI_PMKID_LEN 16U

/* Попытаться извлечь PMKID из кадра EAPOL-Key.
 *
 *   eapol      — указатель на начало EAPOL-кадра (сразу заголовок EAPOL: version,
 *                type, body_length), т.е. то, что wifi_analyzer передаёт как
 *                `payload + eapol_offset`;
 *   eapol_len  — длина доступных байт EAPOL (`eapol_length` у анализатора);
 *   out_pmkid  — буфер на WIFI_PMKID_LEN байт; заполняется ТОЛЬКО при возврате true.
 *
 * Возврат:
 *   true  — найден корректный PMKID-KDE, 16 байт записаны в out_pmkid;
 *   false — не EAPOL-Key, кадр некорректен/обрезан, либо PMKID-KDE отсутствует.
 *
 * Функция НЕ классифицирует M1 — это задача вызывающего (wifi_eapol_classify).
 * Все обращения к байтам защищены границами: битый/короткий ввод даёт false,
 * а не выход за буфер. Состояния не хранит, память не выделяет. */
bool wifi_pmkid_extract_from_eapol(const uint8_t *eapol, size_t eapol_len,
                                   uint8_t out_pmkid[WIFI_PMKID_LEN]);

#ifdef __cplusplus
}
#endif
