#include <assert.h>
#include <stdio.h>
#include "wifi_connection_manager_core.h"
#include "wifi_known_networks_core.h"

static const uint8_t B1[6]={0x10,0,0,0,0,1};
static const uint8_t B2[6]={0x10,0,0,0,0,2};
static const uint8_t B3[6]={0x10,0,0,0,0,3};

int main(void)
{
    wifi_conn_sm_t sm;
    wifi_conn_sm_init(&sm,true);

    /* 1-4: GOT_IP needs the active association boundary. */
    assert(!wifi_conn_sm_got_ip(&sm,1));
    wifi_conn_sm_begin(&sm,1,WIFI_CONN_ORIGIN_AUTO);
    assert(wifi_conn_sm_failed(&sm,1,WIFI_CONN_FAIL_TRANSIENT,1,false));
    assert(sm.phase==WIFI_CONN_PHASE_RETRY_WAIT);
    assert(!wifi_conn_sm_got_ip(&sm,1));
    assert(!wifi_conn_sm_failed(&sm,1,WIFI_CONN_FAIL_TRANSIENT,1,false));
    assert(sm.retry_count==1);
    wifi_conn_sm_begin(&sm,2,WIFI_CONN_ORIGIN_MANUAL);
    assert(!wifi_conn_sm_got_ip(&sm,2));
    assert(wifi_conn_sm_associated(&sm,2));
    assert(wifi_conn_sm_got_ip(&sm,2));

    /* 5-6: disconnect is accepted during association and DHCP wait. */
    wifi_conn_sm_init(&sm,true);
    wifi_conn_sm_begin(&sm,3,WIFI_CONN_ORIGIN_AUTO);
    assert(wifi_conn_sm_failed(&sm,3,WIFI_CONN_FAIL_ASSOCIATION,2,false));
    wifi_conn_sm_begin(&sm,4,WIFI_CONN_ORIGIN_AUTO);
    assert(wifi_conn_sm_associated(&sm,4));
    assert(wifi_conn_sm_failed(&sm,4,WIFI_CONN_FAIL_TRANSIENT,3,false));

    /* 7-8: manual preemption invalidates the old application lifecycle. */
    wifi_conn_sm_begin(&sm,5,WIFI_CONN_ORIGIN_AUTO);
    wifi_conn_sm_begin(&sm,6,WIFI_CONN_ORIGIN_MANUAL);
    assert(!wifi_conn_sm_associated(&sm,5));
    assert(!wifi_conn_sm_got_ip(&sm,5));
    assert(sm.phase==WIFI_CONN_PHASE_ASSOCIATING);

    /* 9-11: timeout closes the lifecycle; retry starts from ASSOCIATING. */
    assert(wifi_conn_sm_associated(&sm,6));
    assert(wifi_conn_sm_timeout(&sm,6));
    assert(!wifi_conn_sm_got_ip(&sm,6));
    wifi_conn_sm_init(&sm,true);
    wifi_conn_sm_begin(&sm,7,WIFI_CONN_ORIGIN_AUTO);
    assert(wifi_conn_sm_timeout(&sm,7));
    assert(!wifi_conn_sm_associated(&sm,7));
    wifi_conn_sm_retry_started(&sm,8);
    assert(sm.phase==WIFI_CONN_PHASE_ASSOCIATING&&sm.attempt_id==8);
    assert(sm.counters.lifecycle_events_rejected>=1);

    wifi_known_network_db_t db;
    wifi_known_db_init(&db);
    assert(wifi_known_db_upsert_success(&db,"IoTSE-LAB","secret",
           WIFI_SECURITY_WPA2_PSK,6,B1,false));
    int known=wifi_known_db_find(&db,"IoTSE-LAB");
    assert(known>=0&&wifi_known_db_set_auto(&db,"IoTSE-LAB",true));
    size_t vi=99;

    /* 12: obvious downgrade is rejected. */
    wifi_known_visible_ap_t open={"IoTSE-LAB",B1,-20,6,WIFI_SECURITY_OPEN};
    assert(wifi_known_db_select_visible(&db,&open,1,&vi)<0);

    /* 13-17: compatible BSSID/channel mobility and deterministic choice. */
    wifi_known_visible_ap_t moved={"IoTSE-LAB",B2,-55,11,WIFI_SECURITY_WPA2_PSK};
    assert(wifi_known_db_select_visible(&db,&moved,1,&vi)==known&&vi==0);
    wifi_known_visible_ap_t aps[3]={
        {"IoTSE-LAB",B1,-70,1,WIFI_SECURITY_WPA2_PSK},
        {"IoTSE-LAB",B2,-35,11,WIFI_SECURITY_WPA2_PSK},
        {"IoTSE-LAB",B3,-35,3,WIFI_SECURITY_WPA2_PSK}};
    assert(wifi_known_db_select_visible(&db,aps,3,&vi)==known&&vi==1);
    assert(wifi_known_db_select_visible(&db,&aps[1],2,&vi)==known&&vi==0);

    /* 18: matching saved BSSID cannot override incompatible security. */
    wifi_known_visible_ap_t mixed[2]={open,moved};
    assert(wifi_known_db_select_visible(&db,mixed,2,&vi)==known&&vi==1);

    /* 19: only non-downgrading transition-mode moves are accepted. */
    assert(wifi_known_security_compatible(WIFI_SECURITY_WPA2_PSK,
                                          WIFI_SECURITY_WPA2_WPA3_PSK));
    assert(wifi_known_security_compatible(WIFI_SECURITY_WPA2_WPA3_PSK,
                                          WIFI_SECURITY_WPA3_PSK));
    assert(!wifi_known_security_compatible(WIFI_SECURITY_WPA3_PSK,
                                           WIFI_SECURITY_WPA2_WPA3_PSK));
    assert(!wifi_known_security_compatible(WIFI_SECURITY_UNKNOWN,
                                           WIFI_SECURITY_UNKNOWN));

    /* Legacy v3 OPEN+password was migrated without auth metadata. */
    wifi_known_network_db_t legacy;
    wifi_known_db_init(&legacy);
    assert(wifi_known_db_upsert_success(&legacy,"legacy","secret",
           WIFI_SECURITY_OPEN,1,B1,false));
    assert(wifi_known_db_set_auto(&legacy,"legacy",true));
    wifi_known_visible_ap_t legacy_wpa2={"legacy",B2,-40,11,WIFI_SECURITY_WPA2_PSK};
    wifi_known_visible_ap_t legacy_open={"legacy",B1,-20,1,WIFI_SECURITY_OPEN};
    assert(wifi_known_db_select_visible(&legacy,&legacy_wpa2,1,&vi)>=0);
    assert(wifi_known_db_select_visible(&legacy,&legacy_open,1,&vi)<0);

    /* 20: no compatible candidate is stable and has no selection side effect. */
    for(unsigned i=0;i<10;i++)assert(wifi_known_db_select_visible(&db,&open,1,&vi)<0);

    puts("wifi_connection_post_audit_unit: PASS (20 targeted cases)");
    return 0;
}
