#include "wifi_scan_lifecycle.h"

#include <string.h>

void wifi_scan_lifecycle_init(wifi_scan_lifecycle_t *scan)
{
    if (scan != NULL) memset(scan, 0, sizeof(*scan));
}

bool wifi_scan_lifecycle_begin(wifi_scan_lifecycle_t *scan, uint32_t id,
                               wifi_scan_owner_t owner)
{
    if (scan == NULL || scan->active_id != 0 || id == 0 ||
        owner == WIFI_SCAN_OWNER_NONE) return false;
    scan->active_id = id;
    scan->deadline_id = id;
    scan->owner = owner;
    return true;
}

bool wifi_scan_lifecycle_preempt_auto(wifi_scan_lifecycle_t *scan,
                                      uint32_t *retired_id)
{
    if (scan == NULL || scan->active_id == 0 ||
        scan->owner != WIFI_SCAN_OWNER_AUTO) return false;
    if (retired_id != NULL) *retired_id = scan->active_id;
    scan->retired_id = scan->active_id;
    scan->active_id = 0;
    scan->deadline_id = 0;
    scan->owner = WIFI_SCAN_OWNER_NONE;
    return true;
}

bool wifi_scan_lifecycle_event_is_current(const wifi_scan_lifecycle_t *scan,
                                          uint32_t id)
{
    return scan != NULL && id != 0 && id == scan->active_id;
}

bool wifi_scan_lifecycle_deadline_is_current(const wifi_scan_lifecycle_t *scan)
{
    return scan != NULL && scan->active_id != 0 &&
           scan->deadline_id == scan->active_id;
}

void wifi_scan_lifecycle_complete(wifi_scan_lifecycle_t *scan)
{
    if (scan == NULL) return;
    scan->active_id = 0;
    scan->deadline_id = 0;
    scan->owner = WIFI_SCAN_OWNER_NONE;
}
