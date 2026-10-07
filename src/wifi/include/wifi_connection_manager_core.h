#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { WIFI_CONN_PHASE_IDLE=0,WIFI_CONN_PHASE_ASSOCIATING,WIFI_CONN_PHASE_WAITING_IP,WIFI_CONN_PHASE_CONNECTED,WIFI_CONN_PHASE_RETRY_WAIT,WIFI_CONN_PHASE_SUSPENDED } wifi_conn_phase_t;
typedef enum { WIFI_CONN_ORIGIN_NONE=0,WIFI_CONN_ORIGIN_MANUAL,WIFI_CONN_ORIGIN_AUTO } wifi_conn_origin_t;
typedef enum { WIFI_CONN_FAIL_NONE=0,WIFI_CONN_FAIL_AP_UNAVAILABLE,WIFI_CONN_FAIL_AUTH,WIFI_CONN_FAIL_ASSOCIATION,WIFI_CONN_FAIL_HANDSHAKE,WIFI_CONN_FAIL_IP_TIMEOUT,WIFI_CONN_FAIL_TRANSIENT,WIFI_CONN_FAIL_USER,WIFI_CONN_FAIL_RADIO } wifi_conn_failure_t;
typedef enum { WIFI_CONN_AUTO_RUNTIME_OFF=0,WIFI_CONN_AUTO_RUNTIME_ERROR,WIFI_CONN_AUTO_RUNTIME_IDLE,WIFI_CONN_AUTO_RUNTIME_RETRY_WAIT,WIFI_CONN_AUTO_RUNTIME_BUSY } wifi_conn_auto_runtime_t;
typedef struct {uint32_t manual_attempts,auto_attempts,successful_connections,auth_failures,association_failures,ip_timeouts,transient_disconnects,retries,stale_events_ignored,lifecycle_events_rejected;} wifi_conn_counters_t;
typedef struct {wifi_conn_phase_t phase;wifi_conn_origin_t origin;uint32_t attempt_id;uint8_t retry_count;bool auto_enabled;bool auto_blocked_credentials;wifi_conn_failure_t last_failure;uint16_t last_reason;wifi_conn_counters_t counters;} wifi_conn_sm_t;
void wifi_conn_sm_init(wifi_conn_sm_t*s,bool auto_enabled);
void wifi_conn_sm_set_auto(wifi_conn_sm_t*s,bool enabled);
void wifi_conn_sm_begin(wifi_conn_sm_t*s,uint32_t id,wifi_conn_origin_t origin);
bool wifi_conn_sm_associated(wifi_conn_sm_t*s,uint32_t id);
bool wifi_conn_sm_got_ip(wifi_conn_sm_t*s,uint32_t id);
bool wifi_conn_sm_failed(wifi_conn_sm_t*s,uint32_t id,wifi_conn_failure_t failure,uint16_t reason,bool was_connected);
bool wifi_conn_sm_timeout(wifi_conn_sm_t*s,uint32_t id);
bool wifi_conn_sm_can_auto(const wifi_conn_sm_t*s);
uint32_t wifi_conn_sm_retry_delay_ms(const wifi_conn_sm_t*s);
void wifi_conn_sm_retry_started(wifi_conn_sm_t*s,uint32_t id);
void wifi_conn_sm_user_disconnect(wifi_conn_sm_t*s);
bool wifi_conn_auto_activation_due(bool allowed,wifi_conn_auto_runtime_t runtime,bool retry_due);
