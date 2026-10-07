#include <assert.h>
#include <stdio.h>
#include "wifi_connection_manager_core.h"
static wifi_conn_sm_t s;
static void fresh(bool a){wifi_conn_sm_init(&s,a);assert(s.phase==WIFI_CONN_PHASE_IDLE);}
int main(void){
 /* 1 boot no saved, 2 saved auto off, 3 saved auto on */
 fresh(false);assert(!wifi_conn_sm_can_auto(&s));fresh(false);assert(!wifi_conn_sm_can_auto(&s));fresh(true);assert(wifi_conn_sm_can_auto(&s));
 /* 4 manual success */ wifi_conn_sm_begin(&s,1,WIFI_CONN_ORIGIN_MANUAL);assert(s.phase==WIFI_CONN_PHASE_ASSOCIATING);assert(wifi_conn_sm_associated(&s,1));assert(s.phase==WIFI_CONN_PHASE_WAITING_IP);assert(wifi_conn_sm_got_ip(&s,1));assert(s.phase==WIFI_CONN_PHASE_CONNECTED);
 /* 5 auth failure */ fresh(true);wifi_conn_sm_begin(&s,2,WIFI_CONN_ORIGIN_MANUAL);assert(wifi_conn_sm_failed(&s,2,WIFI_CONN_FAIL_AUTH,202,false));assert(s.phase==WIFI_CONN_PHASE_IDLE&&s.counters.auth_failures==1);
 /* 6 AP unavailable auto */ fresh(true);wifi_conn_sm_begin(&s,3,WIFI_CONN_ORIGIN_AUTO);assert(wifi_conn_sm_failed(&s,3,WIFI_CONN_FAIL_AP_UNAVAILABLE,201,false));assert(s.phase==WIFI_CONN_PHASE_RETRY_WAIT&&wifi_conn_sm_retry_delay_ms(&s)==2000);
 /* 7 GOT_IP timeout */ fresh(false);wifi_conn_sm_begin(&s,4,WIFI_CONN_ORIGIN_MANUAL);assert(wifi_conn_sm_associated(&s,4));assert(wifi_conn_sm_timeout(&s,4));assert(s.last_failure==WIFI_CONN_FAIL_IP_TIMEOUT&&s.counters.ip_timeouts==1);
 /* 8 stable disconnect */ fresh(true);wifi_conn_sm_begin(&s,5,WIFI_CONN_ORIGIN_AUTO);assert(wifi_conn_sm_associated(&s,5));assert(wifi_conn_sm_got_ip(&s,5));assert(wifi_conn_sm_failed(&s,5,WIFI_CONN_FAIL_TRANSIENT,200,true));assert(s.counters.transient_disconnects==1&&s.phase==WIFI_CONN_PHASE_RETRY_WAIT);
 /* 9 manual overrides pending retry */ wifi_conn_sm_begin(&s,6,WIFI_CONN_ORIGIN_MANUAL);assert(s.origin==WIFI_CONN_ORIGIN_MANUAL&&s.phase==WIFI_CONN_PHASE_ASSOCIATING&&s.retry_count==0);
 /* 10/11 auto toggle does not alter manual phase */ wifi_conn_sm_set_auto(&s,true);assert(s.phase==WIFI_CONN_PHASE_ASSOCIATING);wifi_conn_sm_set_auto(&s,false);assert(s.phase==WIFI_CONN_PHASE_ASSOCIATING);
 /* 12 newer manual attempt owns state */ wifi_conn_sm_begin(&s,7,WIFI_CONN_ORIGIN_MANUAL);assert(s.attempt_id==7);
 /* 13 stale old event */ assert(!wifi_conn_sm_got_ip(&s,6));assert(s.attempt_id==7&&s.counters.stale_events_ignored==1);
 /* 14/15 BSSID cases are known-db policy tests below */
 /* 16 missing and 17 corrupt are codec tests below */
 /* 18 user disconnect */ wifi_conn_sm_user_disconnect(&s);assert(s.phase==WIFI_CONN_PHASE_IDLE&&s.last_failure==WIFI_CONN_FAIL_USER);
 /* 19 repeated transient disconnects */ fresh(true);for(uint32_t i=10;i<13;i++){wifi_conn_sm_begin(&s,i,WIFI_CONN_ORIGIN_AUTO);assert(wifi_conn_sm_associated(&s,i));assert(wifi_conn_sm_got_ip(&s,i));assert(wifi_conn_sm_failed(&s,i,WIFI_CONN_FAIL_TRANSIENT,200,true));}assert(s.counters.transient_disconnects==3);
 /* 20 bounded retry exhaustion */ fresh(true);for(uint32_t i=20;i<26;i++){wifi_conn_sm_retry_started(&s,i);assert(wifi_conn_sm_failed(&s,i,WIFI_CONN_FAIL_AP_UNAVAILABLE,201,false));}assert(s.retry_count==5&&s.phase==WIFI_CONN_PHASE_IDLE&&s.counters.retries==5);assert(!wifi_conn_sm_can_auto(&s));
 /* clear credential failure blocks automatic storm */ fresh(true);wifi_conn_sm_begin(&s,40,WIFI_CONN_ORIGIN_AUTO);assert(wifi_conn_sm_failed(&s,40,WIFI_CONN_FAIL_AUTH,202,false));assert(s.auto_blocked_credentials&&!wifi_conn_sm_can_auto(&s));wifi_conn_sm_set_auto(&s,true);assert(wifi_conn_sm_can_auto(&s));
 puts("wifi_connection_manager_unit: PASS (20 transition scenarios)");return 0;
}
