#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t wifi_capture_export_start(char *url, uint32_t url_size);
void wifi_capture_export_stop(void);
bool wifi_capture_export_is_running(void);
