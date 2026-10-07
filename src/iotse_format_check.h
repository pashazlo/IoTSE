#pragma once

#include "esp_log.h"

/* ESP-IDF 6.2 log v1 declares esp_log() without a printf attribute.  Add the
 * attribute for IoTSE translation units so GCC can validate ESP_LOG* calls. */
#if defined(__GNUC__)
void esp_log(esp_log_config_t config, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
#endif
