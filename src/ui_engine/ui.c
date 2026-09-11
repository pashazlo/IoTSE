#include "ui.h"

#include "display.h"
#include "gfx_canvas.h"

#include "ui_screen.h"
#include "ui_render.h"
#include "ui_controller.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"

// ============================================================================
// Configuration
// ============================================================================

static const char *TAG = "UI";

#define UI_QUEUE_LEN 10

// ============================================================================
// UI State
// ============================================================================

// Очередь событий интерфейса.
// Сюда input_bridge отправляет UI_EVT_UP, UI_EVT_DOWN и другие события.
static QueueHandle_t ui_queue = NULL;
