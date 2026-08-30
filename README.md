# IoTSE - RF Tool for ESP32-S3

IoT проект на базе **ESP32-S3** с полным использованием памяти:
- **16MB Flash** — полный объём памяти модуля
- **8MB PSRAM (Octal)** — внешняя память для приложения
- WiFi + BLE готовы к подключению
- USB-Serial поддержка для отладки

## Характеристики

| Компонент | Конфигурация |
|-----------|-------------|
| Процессор | ESP32-S3 (Xtensa 32-bit) |
| Flash | 16MB |
| PSRAM | 8MB (Octal, 80MHz) |
| Таблица разделов | Кастомная (partitions.csv) |

## Структура проекта

```
.
├── src/
│   ├── main.c              — точка входа приложения
│   └── CMakeLists.txt      — конфиг компилятора
├── sdkconfig.defaults      — конфиг ESP-IDF (память, консоль, WDT)
├── partitions.csv          — таблица разделов Flash
└── .github/workflows/      — CI/CD
```

## Быстрый старт

### Требования
- ESP-IDF v5.0 или выше ([установка](https://docs.espressif.com/projects/esp-idf/))
- Python 3.7+

### Сборка

```bash
idf.py build
```

Полная сборка с merge-bin:
```bash
idf.py build merge-bin
```

### Загрузка на устройство

```bash
# Авто-определение порта
idf.py -p /dev/ttyUSB0 flash monitor

# Только загрузка
idf.py -p /dev/ttyUSB0 flash

# Мониторинг UART
idf.py monitor
```

### Очистка сборки

```bash
idf.py fullclean
```

## Выходные данные

После сборки в директории `build/`:
- `firmware.elf` — скомпилированный образ
- `merged-binary.bin` — полная прошивка для загрузки на устройство

## Конфигурация

Основные опции в `sdkconfig.defaults`:

```ini
# Память
CONFIG_SPIRAM_USE_MALLOC=y              # Использовать PSRAM для malloc
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16k # Порог перемещения в PSRAM

# Консоль
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y   # USB UART вывод

# Стабильность
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10       # Watchdog 10 сек
CONFIG_FREERTOS_TICK_RATE_HZ=1000      # Такт FreeRTOS
```

Для изменения конфига:
```bash
idf.py menuconfig
```

## Логирование

Консоль доступна через USB-Serial на скорости **115200 baud**.

## Лицензия

MIT

## TODO

- [ ] Добавить SD Card поддержку (FAT filesystem)
- [ ] Реализовать RF функциональность
- [ ] Добавить WiFi/BLE примеры
