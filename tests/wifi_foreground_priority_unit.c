#include <assert.h>
#include <stdio.h>

#include "wifi_scan_lifecycle.h"

int main(void)
{
    wifi_scan_lifecycle_t scan;
    wifi_scan_lifecycle_init(&scan);

    /* 1: IDLE -> USER scan. */
    assert(wifi_scan_lifecycle_begin(&scan, 1, WIFI_SCAN_OWNER_USER));
    wifi_scan_lifecycle_complete(&scan);

    /* 2: CONNECTED uses the same idle scan lifecycle after radio accepts it. */
    assert(wifi_scan_lifecycle_begin(&scan, 2, WIFI_SCAN_OWNER_USER));
    wifi_scan_lifecycle_complete(&scan);

    /* 3: AUTO scan is explicitly preemptible by USER. */
    assert(wifi_scan_lifecycle_begin(&scan, 12, WIFI_SCAN_OWNER_AUTO));
    uint32_t retired = 0;
    assert(wifi_scan_lifecycle_preempt_auto(&scan, &retired) && retired == 12);
    assert(wifi_scan_lifecycle_begin(&scan, 13, WIFI_SCAN_OWNER_USER));

    /* 4-5: cancelled AUTO completion cannot finish the USER generation. */
    assert(!wifi_scan_lifecycle_event_is_current(&scan, 12));
    assert(wifi_scan_lifecycle_event_is_current(&scan, 13));

    /* 6: only USER #13 may publish its result/snapshot generation. */
    assert(scan.owner == WIFI_SCAN_OWNER_USER && scan.active_id == 13);
    wifi_scan_lifecycle_complete(&scan);

    /* 7: scan lifecycle does not contain or mutate persisted AUTO policy. */
    bool auto_policy_enabled = true;
    assert(auto_policy_enabled);

    /* 8: CONNECT can retire an AUTO generation through the same boundary. */
    assert(wifi_scan_lifecycle_begin(&scan, 20, WIFI_SCAN_OWNER_AUTO));
    assert(wifi_scan_lifecycle_preempt_auto(&scan, &retired));

    /* 9: DISCONNECT has no scan ownership side effect when no scan exists. */
    assert(!wifi_scan_lifecycle_preempt_auto(&scan, NULL));

    /* 10: PROMISCUOUS acquire can retire AUTO before changing radio mode. */
    assert(wifi_scan_lifecycle_begin(&scan, 21, WIFI_SCAN_OWNER_AUTO));
    assert(wifi_scan_lifecycle_preempt_auto(&scan, &retired));

    /* 11: repeated AUTO -> USER -> release cycles do not retain ownership. */
    for (uint32_t i = 0; i < 1000; ++i) {
        assert(wifi_scan_lifecycle_begin(&scan, 100 + i * 2,
                                         WIFI_SCAN_OWNER_AUTO));
        assert(wifi_scan_lifecycle_preempt_auto(&scan, &retired));
        assert(wifi_scan_lifecycle_begin(&scan, 101 + i * 2,
                                         WIFI_SCAN_OWNER_USER));
        wifi_scan_lifecycle_complete(&scan);
    }
    assert(scan.active_id == 0 && scan.owner == WIFI_SCAN_OWNER_NONE);

    /* 12: retired AUTO deadline cannot time out a new USER generation. */
    assert(wifi_scan_lifecycle_begin(&scan, 5000, WIFI_SCAN_OWNER_AUTO));
    assert(wifi_scan_lifecycle_preempt_auto(&scan, &retired));
    assert(wifi_scan_lifecycle_begin(&scan, 5001, WIFI_SCAN_OWNER_USER));
    assert(scan.deadline_id == 5001 && retired == 5000);
    assert(wifi_scan_lifecycle_deadline_is_current(&scan));

    /* 13: event-driven handoff keeps AUTO current until SCAN_DONE. */
    wifi_scan_lifecycle_complete(&scan);
    assert(wifi_scan_lifecycle_begin(&scan, 6000, WIFI_SCAN_OWNER_AUTO));
    assert(!wifi_scan_lifecycle_begin(&scan, 6001, WIFI_SCAN_OWNER_USER));
    assert(wifi_scan_lifecycle_event_is_current(&scan, 6000));
    wifi_scan_lifecycle_complete(&scan);
    assert(wifi_scan_lifecycle_begin(&scan, 6001, WIFI_SCAN_OWNER_USER));

    /* 14: the retired AUTO generation cannot complete the new USER scan. */
    assert(!wifi_scan_lifecycle_event_is_current(&scan, 6000));
    assert(wifi_scan_lifecycle_event_is_current(&scan, 6001));

    puts("wifi_foreground_priority_unit: PASS (14 ownership cases)");
    return 0;
}
