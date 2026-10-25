# PMKID: план и дизайн

> Статус на 25 октября 2026. Документ описывает фичу PMKID для IoTSE: цель,
> переиспользуемый фундамент, гипотезы по алгоритму, интерфейс, фазы и риски.
> Приоритет у фактического кода и README_CODEX.md, если они разойдутся с планом.

## 1. Что такое PMKID и что значит «кристальный»

PMKID (Pairwise Master Key IDentifier) — 16 байт, которые точка доступа **может**
положить в первый кадр 4-way handshake (**M1**, EAPOL-Key) внутри поля Key Data
как RSN-KDE особого вида:

```
[0xDD][len=0x14][OUI 00-0F-AC][type 0x04][PMKID 16 байт]
```

AP вычисляет его как `HMAC-SHA1-128(PMK, "PMK Name" ‖ AP_MAC ‖ STA_MAC)`. Значение
зависит только от PMK (из пароля+SSID) и двух MAC, поэтому **одного M1 достаточно
для офлайн-подбора пароля** (hashcat mode 22000, префикс `WPA*01`), без полного
хендшейка и без реального клиента («clientless»).

**«Кристальный PMKID»** — валидированная запись ровно из того, что нужно для
подбора, без мусора и двусмысленности:

```
PMKID(16) + AP_MAC + STA_MAC + ESSID + AKM
```

экспортируемая строкой hashcat 22000:

```
WPA*01*<pmkid>*<apmac>*<stamac>*<essid_hex>***
```

Кристальность обеспечивает **строгая валидация**: точный тег/OUI/тип/длина KDE,
точная сверка body_length EAPOL, AKM = PSK, корректный SSID. Сомнительное —
отбрасываем.

## 2. Фундамент (переиспользуем) и что недостаёт

**Готово в проекте:**

- promiscuous-тракт + `wifi_frame_parser` → `is_eapol`, `eapol_offset/length`,
  роли BSSID/STA, канал, RSSI;
- `wifi_eapol_parser` → классификация M1..M4, `key_data_length`, флаги key_info;
- `wifi_eapol_tracker` → сессии по (BSSID, STA), маски M1..M4, replay-счётчики;
- `wifi_rsn_parser` → AKM, `pmkid_count` в RSN-IE (для метки «crackable»);
- `wifi_ap_tracker` → SSID ↔ BSSID (для заполнения ESSID);
- `tx_engine` / `tx_hal_80211` → инъекция кадров (для активного режима);
- `wifi_pcap_codec` / FM worker → безопасное сохранение файлов.

**Точка интеграции:** `wifi_analyzer.c`, ветка `info.is_eapol` — там уже есть
сырые байты EAPOL (`payload + eapol_offset`), длина, BSSID/STA и классификация.

**Недостаёт (новая работа):**

1. **Экстрактор PMKID** из Key Data M1 (парсер не отдаёт байты Key Data).
2. **Хранилище** результатов (ограниченное, с дедупликацией).
3. **(Фаза 2)** построители кадров **auth + assoc-req** (сейчас только beacon/deauth).
4. **UI** страница PMKID + экспорт в hashcat 22000.

## 3. Гипотезы «как строить алгоритм наиболее эффективно»

### Способ добычи M1

- **H1 — пассивный оппортунистический.** Вытаскиваем PMKID из любого M1,
  увиденного при сниффинге. Плюсы: ноль нового TX, безопасно, переиспользует всё,
  работает параллельно с Air Monitor. Минусы: ждём, пока клиент (пере)подключится;
  не «по кнопке».
- **H2 — активный clientless (рекомендован для «по требованию»).** Инжектим
  auth + assoc-req к целевой AP со своего MAC → AP присылает нам M1 с PMKID. Плюсы:
  по требованию, быстро, адресно; каноничный эффективный PMKID-attack; задействует
  TX-движок. Минусы: нужны построители auth/assoc + мини-автомат; радио должно
  стоять на канале AP (фиксация канала — **через `wifi_worker`**, не через движок TX).
- **H3 — через штатный `esp_wifi` connect.** Отвергнут: внутренний supplicant
  съедает EAPOL (M1 можем не увидеть) и сильно дёргает lifecycle радио — конфликт
  с владением `wifi_worker`.

### Эмпирические гипотезы для проверки на железе

- **A:** большинство домашних PSK-AP кладут PMKID в M1 → мерить hit-rate пассивно.
- **B:** активный auth+assoc со своего MAC стабильно вызывает M1-с-PMKID за N мс на
  канале AP → мерить задержку/успех.
- **C:** офлайн-подбор применим только при **AKM = PSK / FT-PSK** (00-0F-AC-02/04);
  не-PSK (SAE, 802.1X) помечать «present, not crackable».

### Рекомендация

Строить **H1 → затем H2** поверх общего store/UI. H3 — нет.

## 4. Интерфейс (эскиз)

```c
/* Фаза 1 — чистый экстрактор (реализовано) */
bool wifi_pmkid_extract_from_eapol(const uint8_t *eapol, size_t eapol_len,
                                   uint8_t out_pmkid[WIFI_PMKID_LEN]);

/* Фаза 2 — хранилище результатов */
typedef struct {
    uint8_t pmkid[16], bssid[6], sta[6];
    char    ssid[33];
    wifi_akm_kind_t akm;      /* метка «crackable» */
    int8_t  rssi; int64_t ts_us;
} wifi_pmkid_record_t;
esp_err_t wifi_pmkid_store_init(void);
bool      wifi_pmkid_store_observe(const wifi_pmkid_record_t *rec); /* true = новая */
bool      wifi_pmkid_store_get_snapshot(wifi_pmkid_snapshot_t *out);
void      wifi_pmkid_store_reset(void);

/* Фаза 2 — активный режим */
esp_err_t tx_frame_build_auth(/* bssid, src_mac */);        /* open-system, seq 1 */
esp_err_t tx_frame_build_assoc_req(/* bssid, ssid, rsn_ie */);
esp_err_t wifi_pmkid_attack_request(const uint8_t bssid[6], uint8_t channel);

/* Экспорт */
esp_err_t wifi_pmkid_export_22000(/* snapshot -> строки -> FM worker */);
```

## 5. Фазы (каждая — проверяемый вертикальный срез)

| # | Шаг | Проверка | Статус |
|---|---|---|---|
| 1 | `wifi_pmkid_parser` (обход KDE, строгая валидация) | host-тест на эталонном M1 | **готово** |
| 2 | `wifi_pmkid_store` + хук в анализатор (H1 пассив) | плата: клиент подключается → PMKID появился | — |
| 3 | UI-страница списка PMKID (snapshot) | плата: SSID/BSSID/PMKID | — |
| 4 | Экспорт 22000 через FM worker (temp+rename) | файл открывается hashcat | — |
| 5 | Построители auth + assoc-req (+host-валидаторы) | host-тест байтов кадра | — |
| 6 | Автомат clientless (фикс канала через `wifi_worker`, инжект, ожидание M1) | плата: по кнопке PMKID за < N мс | — |
| 7 | Классификация PSK/не-PSK по beacon | метка «crackable» корректна | — |

## 6. Фаза 1 — что сделано

Файлы:

- `src/wifi/include/wifi_pmkid_parser.h` — API `wifi_pmkid_extract_from_eapol()`;
- `src/wifi/wifi_pmkid_parser.c` — чистый разбор: EAPOL-заголовок → фикс. тело
  ключа (95 байт, MIC 16) → строгая сверка `body_len == 95 + key_data_length` →
  обход KDE в Key Data, строгий матч OUI 00-0F-AC / type 0x04 / len 20;
- `tests/wifi_pmkid_unit.c` — host-тест (валидный M1, PMKID после другого KDE,
  отсутствие PMKID, неверный OUI/тип, не-EAPOL-Key, рассинхрон длин, NULL/короткий
  ввод, все префиксы);
- строка в `src/CMakeLists.txt`.

Запуск теста (из корня):

```sh
gcc -Wall -Wextra -std=c11 tests/wifi_pmkid_unit.c \
    src/wifi/wifi_pmkid_parser.c -Isrc/wifi/include -o /tmp/pmkid_test \
  && /tmp/pmkid_test
```

Допущение: Key MIC = 16 байт (как и в существующем `wifi_eapol_parser`), поэтому
`key_data_length` на фиксированном смещении. Для целей PMKID (WPA2-PSK) верно.

## 7. Риски и ограничения

- **Владение радио:** активный режим требует фикс канала AP — только через
  `wifi_worker` (паттерн FIXED из Air Monitor); движок TX радио не двигает.
- **Свой MAC:** M1 адресуется нашему STA MAC → `esp_wifi_get_mac`; в promiscuous
  кадры к нам доходят.
- **Не все AP дают PMKID** (PMF-required / чистый SAE / 802.1X) — честно помечать.
- **Память:** store маленький (16–32 записи), без per-frame malloc, snapshot под
  коротким mutex (как `eapol_tracker`).
- **Строгость KDE:** в Key Data бывает несколько KDE и паддинг — матчим только
  точный PMKID-KDE, остальное игнор. Это и даёт «кристальность».
- **Законность:** только своё оборудование и разрешённые тесты. H1 пассивен,
  H2 — только auth/assoc, без deauth.
