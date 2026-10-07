# IoTSE Sub-GHz / CC1101 — архитектура и правила разработки

Этот каталог описывает будущий Sub-GHz-модуль IoTSE. Документ нужен до начала
реализации, чтобы собственный код, импортированные библиотеки и аппаратные
драйверы не смешались в один трудно проверяемый слой.

Целевая платформа — ESP32-S3 N16R8 с CC1101. Модуль предназначен для анализа,
захвата, декодирования, сохранения и воспроизведения радиосигналов на собственном
оборудовании и в разрешённых средах.

## 1. Главный принцип

Sub-GHz обрабатывается как последовательный конвейер:

```text
CC1101 / GDO
    -> RMT RX или packet FIFO
    -> bounded internal buffers
    -> raw capture в PSRAM
    -> нормализация импульсов
    -> поиск и группировка кадров
    -> объединение нескольких наблюдений
    -> FEC / Reed–Solomon
    -> protocol decoder
    -> проверенный результат
    -> сохранение через FM worker
    -> Internal FAT или будущая SD-карта
```

Ни ISR, ни RMT callback, ни CC1101 callback не рисуют UI и не пишут файлы.
UI только отправляет команды и читает опубликованные snapshot.

## 2. Планируемые блоки

### `subghz_hal_cc1101`

Единственный низкоуровневый владелец CC1101:

- SPI register access;
- reset и проверка PARTNUM/VERSION;
- frequency, modulation, data rate и bandwidth;
- RX/TX state transitions;
- packet FIFO;
- конфигурация GDO0/GDO2;
- status registers и аппаратные ошибки.

Остальные модули не вызывают SPI CC1101 напрямую.

### `subghz_rmt_transport`

Критический путь raw RX/TX:

- RMT RX принимает последовательность импульсов с GDO;
- RMT TX аппаратно воспроизводит подготовленные импульсы;
- используются фиксированные internal DMA-capable ping-pong buffers;
- callback только переключает блоки и публикует короткое событие;
- `malloc`, логирование, filesystem и UI в callback запрещены.

Большой raw capture нельзя держать во внутренней RAM. Завершённые RMT-блоки
переносятся worker-задачей в PSRAM.

### `subghz_worker`

Persistent FreeRTOS task и единственный владелец lifecycle Sub-GHz:

```text
OFF
IDLE
CONFIGURING
RX_PACKET
TX_PACKET
RX_RAW
TX_RAW
ANALYZING
STOPPING
ERROR
```

Worker принимает immutable-команды. Одна команда получает один терминальный
результат с `request_id`. Повторный запуск не создаёт новую task.

### `subghz_capture_store`

Bounded-хранилище в PSRAM:

- raw RMT symbols;
- несколько наблюдений одного кадра;
- нормализованные durations;
- bit values и confidence;
- overflow/dropped counters;
- generation опубликованного snapshot.

Хранилище не знает о `/storage` и `/sdcard`. Долговременную запись выполняет
FM worker через зарегистрированный `fm_volume_t`.

### `subghz_frame_sync`

Отвечает только за преобразование raw capture в кандидаты кадров:

- фильтрация коротких выбросов;
- оценка базовой длительности символа;
- нормализация pulse widths;
- поиск preamble и sync;
- выравнивание повторных передач;
- разделение разных кадров внутри одного burst.

Этот модуль не знает о KeeLoq, Reed–Solomon или UI.

### `subghz_frame_fusion`

Объединяет несколько наблюдений одного физического кадра:

- группирует только совместимые наблюдения;
- сравнивает значения и confidence каждой позиции;
- принимает уверенное значение;
- конфликтующие позиции помечает как erasure;
- создаёт небольшой ограниченный набор кандидатов;
- не допускает экспоненциального перебора битов.

Для rolling-code протоколов нельзя объединять два разных сообщения только
потому, что они приняты подряд. Сначала необходимо доказать, что наблюдения
относятся к одному повторяемому кадру внутри одного burst/session.

### `subghz_fec_rs`

Изолированный Reed–Solomon decoder. Этапы:

```text
codeword
  -> syndromes
  -> error/erasure locator polynomial
  -> Chien search
  -> error evaluator polynomial
  -> formal derivative
  -> Forney formula
  -> correction
  -> repeated syndrome verification
```

Правила успешного результата:

- количество ошибок и стираний находится в пределах кода;
- число найденных корней совпадает со степенью locator polynomial;
- в формуле Форни нет недопустимого нулевого знаменателя;
- после исправления все синдромы равны нулю;
- CRC и структура протокола проверяются отдельным следующим слоем.

GF lookup tables хранятся один раз как `const` во Flash. Маленький рабочий
workspace decoder размещается во внутренней RAM и переиспользуется между
syndrome, locator, Chien и Forney. Большие captures остаются в PSRAM.

### `subghz_protocols`

Каждый протокол подключается отдельным descriptor/adapter:

```text
detect
decode
validate
encode
describe
```

Протокол не управляет CC1101, RMT, SPI, файлами или UI. Он получает уже
нормализованный bounded frame и возвращает структурированный результат.

KeeLoq необходимо держать отдельным protocol module. Rolling counter,
фиксированная и hopping-части не должны смешиваться с общим frame fusion.
Ключевой материал не выводится в лог, не хранится в UI-строках и затирается из
временных рабочих буферов после использования.

### `subghz_report`

Формирует versioned report, но не открывает файлы самостоятельно. В отчёт входят:

- версия формата;
- hardware/radio profile;
- frequency и modulation;
- параметры RMT tick;
- capture/session/group IDs;
- число observations;
- overflow/dropped;
- FEC и CRC verification;
- декодированный результат;
- ссылка на raw capture, если он сохранён.

Запись выполняется через FM worker. Один и тот же код должен сохранять отчёт во
внутренний FAT или на SD-карту в зависимости от выбранного `fm_volume_t`.

## 3. Владение памятью

### Внутренняя RAM

Только данные с требованиями по задержкам:

- RMT RX/TX ping-pong buffers;
- DMA descriptors;
- ISR/callback state;
- небольшой event queue;
- текущий decoder workspace;
- активные SPI/RMT операции;
- task stacks.

### PSRAM

Большие данные текущего сеанса:

- raw capture;
- observations;
- нормализованные frames;
- candidate pool;
- analysis arena;
- опубликованный result snapshot.

### Internal FAT и SD

Только долговременные данные:

- raw captures;
- отчёты;
- базы протоколов;
- пользовательские radio profiles;
- экспортированные результаты.

Никакой decoder не должен зависеть от конкретной точки монтирования.

## 4. Состояния буферов

RX-блок всегда имеет одного владельца:

```text
FREE -> RMT_FILLING -> READY -> ANALYZING -> FREE
```

TX-блок:

```text
FREE -> PREPARING -> READY -> RMT_SENDING -> FREE
```

Через FreeRTOS queue передаётся handle/index блока, а не сам raw payload. При
переполнении callback не ждёт: увеличивает `dropped`, помечает capture как
неполный и продолжает работу без выхода за границы массива.

## 5. Ограничения первой реализации

Начальные значения являются бюджетом, а не обещанием окончательного API:

```text
observations per group:  8
candidate frames:       16
maximum frame:         256 bytes
RMT internal staging:  2 x 4 KB RX, 2 x 4 KB TX
raw capture store:     512 KB PSRAM
analysis arena:        256 KB PSRAM
decoded result store:   32 KB PSRAM
```

Все лимиты проверяются до записи. Каждая bounded-структура имеет `count`,
`capacity`, `dropped` и/или `truncated`.

## 6. Проверка восстановленного кадра

CRC не используется как единственное доказательство. Результат считается
полностью проверенным только если:

1. observations принадлежат одной совместимой frame group;
2. выравнивание кадров однозначно;
3. Reed–Solomon correction находится в допустимых пределах;
4. повторные синдромы нулевые;
5. CRC совпадает;
6. длина и обязательные поля протокола допустимы;
7. среди ограниченного candidate pool найден один валидный результат.

Рекомендуемые состояния:

```text
UNRESOLVED
CANDIDATE
RS_CORRECTED
CRC_VALID
VERIFIED_UNIQUE
```

## 7. Общая SPI-шина и UI

Перед подключением CC1101 необходимо зафиксировать реальные GPIO, GDO0/GDO2,
CS и используемый SPI host. Если CC1101 делит SPI с дисплеем:

- SPI mutex покрывает фактическую транзакцию;
- CC1101 FIFO получает приоритет между display DMA-полосами;
- во время критического raw RX/TX UI снижает частоту кадров;
- CC1101 конфигурируется до запуска RMT;
- callback не ждёт SPI mutex;
- длительная запись на SD выполняется отдельной writer task.

## 8. Правила импорта кода с GitHub

Сторонний код нельзя просто копировать в `src/subghz` без происхождения и
лицензии. Каждый импорт располагается отдельно:

```text
third_party/
  library_name/
    UPSTREAM.md
    LICENSE
    исходные файлы без скрытых локальных изменений
```

`UPSTREAM.md` обязательно содержит:

```text
Project: полное название
Repository: точный URL
Revision: commit hash или release tag
Imported: YYYY-MM-DD
License: SPDX identifier или точное название
Purpose: зачем библиотека нужна IoTSE
Local changes: ссылка на patch/adapter или "none"
```

Правила:

1. До импорта проверить лицензию и совместимость с будущей лицензией IoTSE.
2. Фиксировать commit hash, не ссылаться только на плавающую ветку `main`.
3. Не редактировать vendor-код незаметно.
4. Локальные изменения хранить отдельными patch-файлами либо явно описывать.
5. IoTSE обращается к библиотеке через маленький adapter API.
6. Vendor-код не получает прямой доступ к UI, FM, SPI mutex и глобальному state.
7. Перед обновлением upstream запускать известные vectors и сборку ESP-IDF.
8. Не импортировать два разных решения одной задачи без обоснования.
9. Не добавлять библиотеку, если используется только несколько простых функций,
   которые безопаснее реализовать и протестировать локально.

## 9. Требования к комментариям

Комментарии объясняют причину ограничения и владение ресурсом, а не повторяют
очевидную строку C.

Хороший комментарий:

```c
/*
 * RMT callback only transfers block ownership. It must not copy the PSRAM
 * capture, wait for SPI, allocate memory, log payloads, or call the UI.
 */
```

Плохой комментарий:

```c
/* Increase index. */
index++;
```

Для каждой публичной структуры необходимо описать:

- кто создаёт объект;
- кто изменяет объект;
- кто освобождает объект;
- допустима ли PSRAM;
- допустим ли доступ из callback/ISR;
- что происходит при переполнении;
- сохраняются ли указатели после возврата функции.

## 10. Порядок реализации

Каждый этап должен собираться и проверяться отдельно:

1. Проверка wiring и безопасный CC1101 HAL.
2. Packet RX/TX без UI и filesystem.
3. RMT raw RX с bounded internal ping-pong.
4. PSRAM capture store и overflow counters.
5. RMT raw replay из фиксированных staging buffers.
6. Нормализация импульсов и frame sync.
7. Multi-observation grouping и confidence/erasures.
8. Изолированный Reed–Solomon decoder с test vectors.
9. Locator polynomial, Chien search и Forney verification.
10. Protocol adapter API и первый простой протокол.
11. KeeLoq adapter на разрешённых тестовых данных.
12. Versioned report через FM worker.
13. Сохранение на Internal FAT.
14. Регистрация SD как `fm_volume_t` без изменения decoder/scanner.
15. UI после стабилизации backend.

Не объединять CC1101 HAL, RMT transport, Reed–Solomon и UI в один широкий патч.

## 11. Зафиксированные уравнения Reed–Solomon

В коде используется поле `GF(2^8)`. Сложение и вычитание в таком поле — одна
и та же операция XOR:

```text
a + b = a - b = a XOR b
```

Пусть `alpha` — примитивный элемент поля, `b` — номер первого корня кода,
`r` — число проверочных символов, а `R(x)` — принятое кодовое слово.

Синдромы:

```text
S_j = R(alpha^(b + j)),  j = 0 .. r-1
S(x) = S_0 + S_1*x + ... + S_(r-1)*x^(r-1)
```

Для ошибок с полевыми локаторами `X_i` полином локаторов имеет вид:

```text
Lambda(x) = product(1 - X_i*x)
```

В `GF(2^8)` знак минус эквивалентен плюсу. Полином оценщика ошибок:

```text
Omega(x) = [S(x) * Lambda(x)] mod x^r
```

Корни ищутся перебором Чиена. Для позиции `p` в массиве длины `n`, где
`p=0` — левый символ:

```text
location_number = n - 1 - p
X_i = alpha^location_number
Lambda(X_i^-1) = 0
```

Формальная производная в поле характеристики 2 содержит только нечётные
коэффициенты исходного полинома:

```text
Lambda'(x) = lambda_1 + lambda_3*x^2 + lambda_5*x^4 + ...
```

Формула Форни для величины ошибки:

```text
E_i = X_i^(1-b) * Omega(X_i^-1) / Lambda'(X_i^-1)
```

После исправления `codeword[p] = codeword[p] XOR E_i` синдромы вычисляются
заново. Результат нельзя объявлять корректным, если число корней не совпало со
степенью `Lambda`, знаменатель Форни равен нулю или повторные синдромы ненулевые.
Даже нулевые синдромы не заменяют отдельную проверку CRC и структуры протокола.

Реализация этих формул находится в:

```text
src/subghz/include/subghz_rs_math.h
src/subghz/subghz_rs_math.c
```

Коэффициенты всех полиномов в API хранятся от младшей степени к старшей:
`polynomial[i]` соответствует `x^i`. Кодовое слово, напротив, хранится в
естественном сетевом порядке: левый, старший символ имеет индекс 0.

## 12. Кандидаты с GitHub для будущего CC1101 HAL

Код этих проектов сейчас не импортирован. Ссылки нужны для технического
сравнения перед написанием нашего адаптера.

1. `nopnop2002/esp-idf-cc1101`
   - нативный компонент ESP-IDF;
   - заявлена поддержка ESP-IDF 5.x и ESP32-S3;
   - есть доступ к регистрам, packet RX/TX и примеры настройки диапазонов;
   - наиболее близкий кандидат для изучения и переноса небольшого HAL;
   - перед импортом отдельно проверить файл LICENSE и зафиксировать commit hash.
   - https://github.com/nopnop2002/esp-idf-cc1101

2. `jgromes/RadioLib`
   - зрелый проект с MIT-лицензией и поддержкой CC1101;
   - хороший источник для проверки расчёта регистров и поведения radio state;
   - библиотека крупная, написана на C++ и имеет собственный HAL, поэтому
     целиком подключать её к текущей C-архитектуре IoTSE пока не стоит.
   - https://github.com/jgromes/RadioLib

3. `devcexx/cc1101-idf`
   - небольшой низкоуровневый ESP-IDF-драйвер с MIT-лицензией;
   - удобен как короткий пример SPI API;
   - проект сам обозначает реализацию как незавершённую, поэтому использовать
     его как основу без собственного тестирования нельзя.
   - https://github.com/devcexx/cc1101-idf

Для первого аппаратного этапа лучше написать собственный тонкий HAL поверх уже
существующего `components/spi_bus`, сверяя последовательности регистров с
datasheet TI и двумя независимыми реализациями. Это сохранит единое владение
общей SPI-шиной, которое сторонняя библиотека не знает.
