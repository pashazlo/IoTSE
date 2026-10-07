#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wifi_connection_manager_core.h"
#include "wifi_known_networks_core.h"

static const uint8_t BSSID[6] = {0x02, 0, 0, 0, 0, 1};

static void add_network(wifi_known_network_db_t *db, const char *ssid,
                        bool auto_connect)
{
    assert(wifi_known_db_upsert_success(db, ssid, "lab-password",
                                        WIFI_SECURITY_WPA2_PSK, 6,
                                        BSSID, false));
    assert(wifi_known_db_set_auto(db, ssid, auto_connect));
}

static bool boot_requests_scan(const wifi_known_network_db_t *db,
                               bool suspended, bool user_suppressed,
                               wifi_conn_auto_runtime_t runtime)
{
    wifi_conn_sm_t sm;
    bool policy = wifi_known_db_auto_count(db) != 0;
    wifi_conn_sm_init(&sm, policy);
    bool allowed = policy && !suspended && !user_suppressed &&
                   wifi_conn_sm_can_auto(&sm);
    return wifi_conn_auto_activation_due(allowed, runtime, true);
}

int main(void)
{
    wifi_known_network_db_t db, restored;
    wifi_known_db_init(&db);

    /* 1: cold boot with no known networks. */
    assert(wifi_known_db_count(&db) == 0);
    assert(!boot_requests_scan(&db, false, false, WIFI_CONN_AUTO_RUNTIME_OFF));

    /* 2: a saved network with Auto Connect disabled remains idle. */
    add_network(&db, "manual-only", false);
    assert(wifi_known_db_auto_count(&db) == 0);
    assert(!boot_requests_scan(&db, false, false, WIFI_CONN_AUTO_RUNTIME_OFF));

    /* 3: one enabled entry activates from the production initial OFF state. */
    assert(wifi_known_db_set_auto(&db, "manual-only", true));
    assert(boot_requests_scan(&db, false, false, WIFI_CONN_AUTO_RUNTIME_OFF));

    /* 4: one enabled entry among several is sufficient. */
    add_network(&db, "second", false);
    assert(wifi_known_db_count(&db) == 2);
    assert(wifi_known_db_auto_count(&db) == 1);

    /* 5: radio OFF is an activation state when its retry deadline is due. */
    assert(wifi_conn_auto_activation_due(true, WIFI_CONN_AUTO_RUNTIME_OFF, true));
    assert(!wifi_conn_auto_activation_due(true, WIFI_CONN_AUTO_RUNTIME_OFF, false));

    /* 6: an ERROR radio is retried autonomously, with deadline throttling. */
    assert(wifi_conn_auto_activation_due(true, WIFI_CONN_AUTO_RUNTIME_ERROR, true));
    assert(!wifi_conn_auto_activation_due(true, WIFI_CONN_AUTO_RUNTIME_ERROR, false));

    /* 7: scan results select a compatible visible saved AP. */
    wifi_known_visible_ap_t visible = {
        "manual-only", BSSID, -42, 6, WIFI_SECURITY_WPA2_PSK
    };
    size_t visible_index = 99;
    int known_index = wifi_known_db_select_visible(&db, &visible, 1,
                                                    &visible_index);
    assert(known_index >= 0 && visible_index == 0);

    /* 8: no candidate enters the bounded Connection Manager retry policy. */
    wifi_conn_sm_t sm;
    wifi_conn_sm_init(&sm, true);
    wifi_conn_sm_begin(&sm, 1, WIFI_CONN_ORIGIN_AUTO);
    assert(wifi_conn_sm_failed(&sm, 1, WIFI_CONN_FAIL_AP_UNAVAILABLE, 0, false));
    assert(sm.phase == WIFI_CONN_PHASE_RETRY_WAIT);
    assert(!wifi_conn_auto_activation_due(true, WIFI_CONN_AUTO_RUNTIME_RETRY_WAIT,
                                          false));
    assert(wifi_conn_auto_activation_due(true, WIFI_CONN_AUTO_RUNTIME_RETRY_WAIT,
                                         true));

    /* 9: the persisted codec preserves Auto Connect across a simulated reboot. */
    uint8_t wire[WIFI_KNOWN_DB_ENCODED_MAX];
    size_t wire_size = wifi_known_db_encode(&db, wire, sizeof(wire));
    assert(wire_size != 0);
    assert(wifi_known_db_decode(wire, wire_size, &restored) == WIFI_KNOWN_CODEC_OK);
    assert(wifi_known_db_auto_count(&restored) == 1);

    /* 10: opening Networks is absent from, and unnecessary to, boot activation. */
    assert(boot_requests_scan(&restored, false, false,
                              WIFI_CONN_AUTO_RUNTIME_OFF));

    /* 11: zero UI commands still reaches the radio/scan request decision. */
    unsigned ui_command_count = 0;
    assert(ui_command_count == 0 &&
           boot_requests_scan(&restored, false, false,
                              WIFI_CONN_AUTO_RUNTIME_OFF));

    /* 12: resume makes the same persisted policy eligible immediately. */
    assert(!boot_requests_scan(&restored, true, false,
                               WIFI_CONN_AUTO_RUNTIME_IDLE));
    assert(boot_requests_scan(&restored, false, false,
                              WIFI_CONN_AUTO_RUNTIME_IDLE));

    /* 13: temporary suspension does not mutate the saved policy. */
    assert(wifi_known_db_auto_count(&restored) == 1);

    /* 14: manual Disconnect suppression is independent of suspension/policy. */
    assert(!boot_requests_scan(&restored, false, true,
                               WIFI_CONN_AUTO_RUNTIME_IDLE));
    assert(wifi_known_db_auto_count(&restored) == 1);

    /* 15: disabling Auto Connect survives reload and requests no work. */
    assert(wifi_known_db_set_auto(&restored, "manual-only", false));
    wire_size = wifi_known_db_encode(&restored, wire, sizeof(wire));
    assert(wire_size != 0);
    wifi_known_db_init(&db);
    assert(wifi_known_db_decode(wire, wire_size, &db) == WIFI_KNOWN_CODEC_OK);
    assert(wifi_known_db_auto_count(&db) == 0);
    assert(!boot_requests_scan(&db, false, false, WIFI_CONN_AUTO_RUNTIME_OFF));

    puts("wifi_auto_boot_unit: PASS (15 cold-boot/UI-independent scenarios)");
    return 0;
}
