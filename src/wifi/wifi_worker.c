#include "wifi_worker.h"
#include "wifi_analyzer.h"
#include "wifi_channel_hopper_internal.h"
#include "wifi_credentials.h"
#include "wifi_promiscuous.h"
#include "wifi_scan_lifecycle.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CMD_QUEUE_LEN 8U
#define RESULT_QUEUE_LEN 8U
#define RADIO_QUEUE_LEN 16U
#define WORKER_STACK 6144U
#define WORKER_PRIORITY 3U
#define SCAN_TIMEOUT_MS 20000U
#define ASSOC_TIMEOUT_MS 15000U
#define DHCP_TIMEOUT_MS 15000U
#define DISCONNECT_TIMEOUT_MS 5000U

typedef enum { RADIO_SCAN_DONE=0, RADIO_CONNECTION } radio_event_kind_t;
typedef struct { radio_event_kind_t kind; uint32_t scan_id; bool scan_success; wifi_radio_connection_event_t connection; } radio_event_t;
typedef enum { FOREGROUND_BARRIER_NONE=0, FOREGROUND_BARRIER_SCAN,
               FOREGROUND_BARRIER_DISCONNECT } foreground_barrier_t;
typedef struct {
    wifi_scan_data_t scan;
    wifi_known_visible_ap_t visible[WIFI_SCAN_MAX_APS];
} wifi_worker_scan_scratch_t;
static const char *TAG="WIFI_CONN";
static QueueHandle_t s_commands,s_results,s_radio_events;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static atomic_int s_state=WIFI_WORKER_STATE_OFF;
static atomic_uint_fast32_t s_next_request=1,s_pending_requests,s_next_attempt=1,s_next_scan=1,s_dropped_events;
static atomic_bool s_connected,s_auto_suspended,s_auto_kick,
                   s_auto_resume_requested;
static atomic_uchar s_first_channel,s_last_channel;
static wifi_scan_snapshot_t s_scan_snapshot;
static wifi_connection_snapshot_t s_connection_snapshot;
static wifi_known_network_db_t s_known, s_known_snapshot;
static wifi_conn_sm_t s_sm, s_sm_snapshot;
static wifi_worker_cmd_t s_active_command,s_switch_target,s_active_scan_command;
static wifi_worker_cmd_t s_foreground_command;
static uint32_t s_active_attempt;
static wifi_scan_lifecycle_t s_scan;
static uint8_t s_associated_channel;
static uint8_t s_associated_bssid[6];
static bool s_active_manual,s_switch_pending,s_internal_disconnect,s_user_suppressed;
static bool s_foreground_pending;
static foreground_barrier_t s_foreground_barrier;
static TickType_t s_deadline,s_retry_at;
static TickType_t s_scan_snapshot_tick;
/* Single-owner scratch allocated before task start. Only wifi_conn_mgr uses it;
 * visible[] pointers remain valid through candidate selection and are not retained. */
static wifi_worker_scan_scratch_t *s_scan_scratch;

static const char*phase_name(wifi_worker_state_t v);
static void stack_checkpoint(const char *stage)
{
    ESP_LOGI(TAG,"STACK stage=%s min_free=%u bytes",stage,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
static bool reached(TickType_t now,TickType_t deadline){return(int32_t)(now-deadline)>=0;}
static void deadline(uint32_t ms){s_deadline=xTaskGetTickCount()+pdMS_TO_TICKS(ms);}
static void clear_deadline(void){s_deadline=0;}
static wifi_worker_state_t state(void){return(wifi_worker_state_t)atomic_load_explicit(&s_state,memory_order_acquire);}
static void set_state(wifi_worker_state_t v){int old=atomic_exchange_explicit(&s_state,(int)v,memory_order_acq_rel);if(old!=(int)v)ESP_LOGI(TAG,"STATE %s -> %s",phase_name((wifi_worker_state_t)old),phase_name(v));}
static wifi_conn_auto_runtime_t auto_runtime(void){switch(state()){case WIFI_WORKER_STATE_OFF:return WIFI_CONN_AUTO_RUNTIME_OFF;case WIFI_WORKER_STATE_ERROR:return WIFI_CONN_AUTO_RUNTIME_ERROR;case WIFI_WORKER_STATE_IDLE:return WIFI_CONN_AUTO_RUNTIME_IDLE;case WIFI_WORKER_STATE_RETRY_WAIT:return WIFI_CONN_AUTO_RUNTIME_RETRY_WAIT;default:return WIFI_CONN_AUTO_RUNTIME_BUSY;}}
static const char*phase_name(wifi_worker_state_t v){switch(v){case WIFI_WORKER_STATE_OFF:return"OFF";case WIFI_WORKER_STATE_STARTING:return"STARTING";case WIFI_WORKER_STATE_IDLE:return"IDLE";case WIFI_WORKER_STATE_SCANNING:return"SCANNING";case WIFI_WORKER_STATE_ASSOCIATING:return"ASSOCIATING";case WIFI_WORKER_STATE_WAITING_IP:return"WAITING_IP";case WIFI_WORKER_STATE_RETRY_WAIT:return"RETRY_WAIT";case WIFI_WORKER_STATE_DISCONNECTING:return"DISCONNECTING";case WIFI_WORKER_STATE_SWITCHING_NETWORK:return"SWITCHING_NETWORK";case WIFI_WORKER_STATE_CONNECTED:return"CONNECTED";case WIFI_WORKER_STATE_PROMISCUOUS:return"PROMISCUOUS";case WIFI_WORKER_STATE_STOPPING:return"STOPPING";case WIFI_WORKER_STATE_ERROR:return"ERROR";default:return"UNKNOWN";}}
static bool any_auto(void){for(unsigned i=0;i<WIFI_KNOWN_NETWORK_MAX;i++)if(s_known.entries[i].occupied&&s_known.entries[i].auto_connect)return true;return false;}
static bool auto_allowed(void){return any_auto()&&!s_user_suppressed&&!atomic_load_explicit(&s_auto_suspended,memory_order_acquire)&&wifi_conn_sm_can_auto(&s_sm);}
static void refresh_channels(void){uint8_t a,b;if(wifi_radio_get_channel_range(&a,&b)==ESP_OK){atomic_store(&s_first_channel,a);atomic_store(&s_last_channel,b);}}
static void publish_policy_snapshots(void){if(s_mutex&&xSemaphoreTake(s_mutex,portMAX_DELAY)==pdTRUE){s_known_snapshot=s_known;s_sm_snapshot=s_sm;xSemaphoreGive(s_mutex);}}
static void publish_connection(bool connected){wifi_connection_info_t info={0};if(connected&&wifi_radio_get_connection_info(&info)!=ESP_OK)connected=false;info.connected=connected;if(xSemaphoreTake(s_mutex,portMAX_DELAY)==pdTRUE){s_connection_snapshot.info=info;s_connection_snapshot.generation++;xSemaphoreGive(s_mutex);}}
static void pending_done(void){uint_fast32_t n=atomic_load(&s_pending_requests);while(n&& !atomic_compare_exchange_weak(&s_pending_requests,&n,n-1U)){} }
static void result(wifi_worker_result_type_t type,const wifi_worker_cmd_t*cmd,esp_err_t err,uint16_t reason,wifi_conn_failure_t failure,uint32_t attempt){if(!cmd||cmd->request_id==0)return;wifi_worker_result_t r={.type=type,.command=cmd->type,.request_id=cmd->request_id,.snapshot_generation=s_scan_snapshot.generation,.error=err,.channel=cmd->type==WIFI_WORKER_CMD_SET_CHANNEL?cmd->data.channel:0,.disconnect_reason=reason,.failure_class=failure,.attempt_id=attempt};if(xQueueSend(s_results,&r,0)!=pdTRUE)ESP_LOGE(TAG,"result queue full request=%"PRIu32,cmd->request_id);pending_done();}
static wifi_conn_failure_t classify(uint16_t r){switch(r){case WIFI_REASON_NO_AP_FOUND:case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:return WIFI_CONN_FAIL_AP_UNAVAILABLE;case WIFI_REASON_AUTH_FAIL:return WIFI_CONN_FAIL_AUTH;case WIFI_REASON_ASSOC_FAIL:return WIFI_CONN_FAIL_ASSOCIATION;case WIFI_REASON_HANDSHAKE_TIMEOUT:case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:return WIFI_CONN_FAIL_HANDSHAKE;default:return WIFI_CONN_FAIL_TRANSIENT;}}
static const char*failure_name(wifi_conn_failure_t f){switch(f){case WIFI_CONN_FAIL_AP_UNAVAILABLE:return"AP_UNAVAILABLE";case WIFI_CONN_FAIL_AUTH:return"AUTH_FAILURE";case WIFI_CONN_FAIL_ASSOCIATION:return"ASSOCIATION_FAILURE";case WIFI_CONN_FAIL_HANDSHAKE:return"HANDSHAKE_TIMEOUT";case WIFI_CONN_FAIL_IP_TIMEOUT:return"IP_TIMEOUT";case WIFI_CONN_FAIL_TRANSIENT:return"TRANSIENT";case WIFI_CONN_FAIL_USER:return"USER";case WIFI_CONN_FAIL_RADIO:return"RADIO";default:return"NONE";}}
static void log_app_action_during_connect(const wifi_worker_cmd_t *cmd)
{
    wifi_worker_state_t current=state();
    if(current!=WIFI_WORKER_STATE_ASSOCIATING&&
       current!=WIFI_WORKER_STATE_WAITING_IP)return;
    const char *action=NULL;
    if(cmd->type==WIFI_WORKER_CMD_SCAN)action="scan";
    else if(cmd->type==WIFI_WORKER_CMD_DISCONNECT)action="disconnect";
    else if(cmd->type==WIFI_WORKER_CMD_SET_CHANNEL)action="set_channel";
    if(action)ESP_LOGI(TAG,"APP_RADIO action=%s request=%"PRIu32
                       " during_attempt=%"PRIu32" state=%s",
                       action,cmd->request_id,s_active_attempt,
                       phase_name(current));
}
static void radio_scan_done(uint32_t id,bool ok,void*ctx){(void)ctx;radio_event_t e={.kind=RADIO_SCAN_DONE,.scan_id=id,.scan_success=ok};if(!s_radio_events||xQueueSend(s_radio_events,&e,0)!=pdTRUE)atomic_fetch_add(&s_dropped_events,1);}
static void radio_connection(const wifi_radio_connection_event_t*event,void*ctx){(void)ctx;if(!event)return;radio_event_t e={.kind=RADIO_CONNECTION,.connection=*event};if(!s_radio_events||xQueueSend(s_radio_events,&e,0)!=pdTRUE)atomic_fetch_add(&s_dropped_events,1);}
static void command(const wifi_worker_cmd_t*c);
static esp_err_t ensure_radio(void);
static void run_foreground_after_barrier(void)
{
    s_foreground_barrier=FOREGROUND_BARRIER_NONE;
    clear_deadline();
    set_state(WIFI_WORKER_STATE_IDLE);
    if(!s_foreground_pending)return;
    wifi_worker_cmd_t pending=s_foreground_command;
    s_foreground_pending=false;
    memset(&s_foreground_command,0,sizeof(s_foreground_command));
    ESP_LOGI(TAG,"foreground handoff complete command=%u request=%"PRIu32,
             (unsigned)pending.type,pending.request_id);
    command(&pending);
    memset(&pending,0,sizeof(pending));
}

static bool foreground_command_type(wifi_worker_cmd_type_t type)
{
    return type==WIFI_WORKER_CMD_SCAN||type==WIFI_WORKER_CMD_CONNECT||
           type==WIFI_WORKER_CMD_DISCONNECT||
           type==WIFI_WORKER_CMD_ENABLE_PROMISCUOUS;
}

static bool defer_foreground_if_needed(const wifi_worker_cmd_t *cmd)
{
    if(!foreground_command_type(cmd->type))return false;
    if(s_foreground_barrier!=FOREGROUND_BARRIER_NONE){
        if(!s_foreground_pending){
            s_foreground_command=*cmd;
            s_foreground_pending=true;
            ESP_LOGI(TAG,"foreground queued command=%u request=%"PRIu32
                     " barrier=%u",(unsigned)cmd->type,cmd->request_id,
                     (unsigned)s_foreground_barrier);
        }else{
            result(WIFI_WORKER_RESULT_ERROR,cmd,ESP_ERR_INVALID_STATE,0,
                   WIFI_CONN_FAIL_RADIO,s_active_attempt);
        }
        return true;
    }
    if(state()==WIFI_WORKER_STATE_SCANNING&&
       s_scan.owner==WIFI_SCAN_OWNER_AUTO){
        s_foreground_command=*cmd;
        s_foreground_pending=true;
        s_foreground_barrier=FOREGROUND_BARRIER_SCAN;
        esp_err_t err=wifi_radio_scan_cancel();
        ESP_LOGI(TAG,"AUTO scan #%"PRIu32" cancel for foreground command=%u"
                 " request=%"PRIu32" driver=%s",s_scan.active_id,
                 (unsigned)cmd->type,cmd->request_id,esp_err_to_name(err));
        if(err!=ESP_OK){
            s_foreground_pending=false;
            s_foreground_barrier=FOREGROUND_BARRIER_NONE;
            memset(&s_foreground_command,0,sizeof(s_foreground_command));
            result(WIFI_WORKER_RESULT_ERROR,cmd,err,0,WIFI_CONN_FAIL_RADIO,0);
        }else deadline(DISCONNECT_TIMEOUT_MS);
        return true;
    }
    if(!s_active_manual&&(state()==WIFI_WORKER_STATE_ASSOCIATING||
                          state()==WIFI_WORKER_STATE_WAITING_IP)){
        s_foreground_command=*cmd;
        s_foreground_pending=true;
        s_foreground_barrier=FOREGROUND_BARRIER_DISCONNECT;
        esp_err_t err=wifi_radio_disconnect();
        ESP_LOGI(TAG,"AUTO connect #%"PRIu32" cancel for foreground command=%u"
                 " request=%"PRIu32" driver=%s",s_active_attempt,
                 (unsigned)cmd->type,cmd->request_id,esp_err_to_name(err));
        if(err==ESP_OK){set_state(WIFI_WORKER_STATE_DISCONNECTING);deadline(DISCONNECT_TIMEOUT_MS);}
        else{
            (void)wifi_radio_stop();set_state(WIFI_WORKER_STATE_OFF);
            s_active_attempt=0;s_active_manual=false;
            if(ensure_radio()==ESP_OK)run_foreground_after_barrier();
            else{result(WIFI_WORKER_RESULT_ERROR,cmd,err,0,WIFI_CONN_FAIL_RADIO,0);s_foreground_pending=false;s_foreground_barrier=FOREGROUND_BARRIER_NONE;memset(&s_foreground_command,0,sizeof(s_foreground_command));}
        }
        return true;
    }
    if(state()==WIFI_WORKER_STATE_RETRY_WAIT){
        s_retry_at=0;s_sm.phase=WIFI_CONN_PHASE_IDLE;s_sm.origin=WIFI_CONN_ORIGIN_NONE;
        set_state(WIFI_WORKER_STATE_IDLE);
    }
    return false;
}
static esp_err_t ensure_radio(void){if(state()!=WIFI_WORKER_STATE_OFF&&state()!=WIFI_WORKER_STATE_ERROR)return ESP_OK;if(state()==WIFI_WORKER_STATE_ERROR)(void)wifi_radio_stop();set_state(WIFI_WORKER_STATE_STARTING);esp_err_t e=wifi_radio_start(radio_scan_done,NULL);if(e==ESP_OK){wifi_radio_set_connect_callback(radio_connection,NULL);refresh_channels();set_state(WIFI_WORKER_STATE_IDLE);}else set_state(WIFI_WORKER_STATE_ERROR);return e;}
static void schedule_retry(void){uint32_t ms=wifi_conn_sm_retry_delay_ms(&s_sm);if(ms==0)ms=5000;s_retry_at=xTaskGetTickCount()+pdMS_TO_TICKS(ms);set_state(WIFI_WORKER_STATE_RETRY_WAIT);ESP_LOGI(TAG,"AUTO retry=%u delay=%"PRIu32"ms",s_sm.retry_count,ms);}
static esp_err_t start_attempt(const wifi_worker_cmd_t*cmd,wifi_conn_origin_t origin){uint32_t id=(uint32_t)atomic_fetch_add(&s_next_attempt,1);if(id==0)id=(uint32_t)atomic_fetch_add(&s_next_attempt,1);wifi_worker_state_t before=state();wifi_conn_sm_begin(&s_sm,id,origin);s_active_attempt=id;s_active_manual=origin==WIFI_CONN_ORIGIN_MANUAL;s_active_command=s_active_manual?*cmd:(wifi_worker_cmd_t){0};const uint8_t*lock=cmd->data.connect.bssid_lock?cmd->data.connect.bssid:NULL;if(cmd->data.connect.candidate_known)ESP_LOGI(TAG,"SELECTED attempt=%"PRIu32" origin=%s bssid="MACSTR" channel=%u rssi=%d auth=%s age=%"PRIu32"ms",id,s_active_manual?"USER":"AUTO",MAC2STR(cmd->data.connect.bssid),(unsigned)cmd->data.connect.channel,(int)cmd->data.connect.candidate_rssi,wifi_security_name(cmd->data.connect.security),cmd->data.connect.candidate_age_ms);else ESP_LOGI(TAG,"SELECTED attempt=%"PRIu32" origin=%s bssid=unknown channel=unknown rssi=unknown auth=unknown age=unknown",id,s_active_manual?"USER":"AUTO");ESP_LOGI(TAG,"CONN #%"PRIu32" %s request=%"PRIu32" %s -> ASSOCIATING ssid='%s' bssid_lock=%u password_len=%u",id,s_active_manual?"MANUAL":"AUTO",cmd->request_id,phase_name(before),cmd->data.connect.ssid,lock?1U:0U,(unsigned)strnlen(cmd->data.connect.password,WIFI_PASSWORD_MAX_LEN+1U));esp_err_t e=wifi_radio_connect(cmd->data.connect.ssid,cmd->data.connect.password,lock,id);ESP_LOGI(TAG,"CONN #%"PRIu32" esp_wifi_connect=%s",id,esp_err_to_name(e));if(e!=ESP_OK){(void)wifi_conn_sm_failed(&s_sm,id,WIFI_CONN_FAIL_RADIO,0,false);if(s_active_manual)result(WIFI_WORKER_RESULT_ERROR,&s_active_command,e,0,WIFI_CONN_FAIL_RADIO,id);else if(auto_allowed())schedule_retry();memset(&s_active_command,0,sizeof(s_active_command));s_active_manual=false;s_active_attempt=0;set_state(WIFI_WORKER_STATE_IDLE);return e;}set_state(WIFI_WORKER_STATE_ASSOCIATING);deadline(ASSOC_TIMEOUT_MS);return ESP_OK;}
static void save_success(const wifi_worker_cmd_t*cmd){wifi_connection_info_t i={0};if(wifi_radio_get_connection_info(&i)!=ESP_OK)return;esp_err_t e=wifi_credentials_upsert_success(&s_known,cmd->data.connect.ssid,cmd->data.connect.password,cmd->data.connect.security,cmd->data.connect.channel,i.bssid);if(e!=ESP_OK)ESP_LOGW(TAG,"known-network save failed: %s",esp_err_to_name(e));}
static void auto_scan_start(void)
{
    if(!auto_allowed())return;
    stack_checkpoint("auto_scan_start");
    if(state()==WIFI_WORKER_STATE_OFF||state()==WIFI_WORKER_STATE_ERROR){
        if(ensure_radio()!=ESP_OK){s_retry_at=xTaskGetTickCount()+pdMS_TO_TICKS(5000);return;}
    }
    if(state()!=WIFI_WORKER_STATE_IDLE&&state()!=WIFI_WORKER_STATE_RETRY_WAIT)return;
    wifi_scan_options_t o={.channel=0,.show_hidden=false,.passive=false,.dwell_ms=0};
    uint32_t id=(uint32_t)atomic_fetch_add(&s_next_scan,1);
    if(id==0)id=(uint32_t)atomic_fetch_add(&s_next_scan,1);
    esp_err_t e=wifi_radio_scan_start(&o,id);
    if(e==ESP_OK){
        (void)wifi_scan_lifecycle_begin(&s_scan,id,WIFI_SCAN_OWNER_AUTO);
        set_state(WIFI_WORKER_STATE_SCANNING);
        deadline(SCAN_TIMEOUT_MS);
        ESP_LOGI(TAG,"AUTO scan #%"PRIu32" started",id);
    }else{
        s_retry_at=xTaskGetTickCount()+pdMS_TO_TICKS(5000);
        set_state(WIFI_WORKER_STATE_RETRY_WAIT);
    }
}
static void auto_no_candidate(void){uint32_t id=(uint32_t)atomic_fetch_add(&s_next_attempt,1);wifi_conn_sm_begin(&s_sm,id,WIFI_CONN_ORIGIN_AUTO);(void)wifi_conn_sm_failed(&s_sm,id,WIFI_CONN_FAIL_AP_UNAVAILABLE,WIFI_REASON_NO_AP_FOUND,false);if(s_sm.phase==WIFI_CONN_PHASE_RETRY_WAIT)schedule_retry();else{set_state(WIFI_WORKER_STATE_IDLE);ESP_LOGW(TAG,"AUTO retry budget exhausted");}ESP_LOGI(TAG,"AUTO skipped: no visible known network");}
static void finish_scan(const radio_event_t*e)
{
    stack_checkpoint("scan_done_enter");
    if(!wifi_scan_lifecycle_event_is_current(&s_scan,e->scan_id)){
        s_sm.counters.stale_events_ignored++;
        ESP_LOGW(TAG,"stale scan event id=%"PRIu32" active=%"PRIu32,
                 e->scan_id,s_scan.active_id);
        return;
    }
    if(s_foreground_barrier==FOREGROUND_BARRIER_SCAN){
        memset(&s_scan_scratch->scan,0,sizeof(s_scan_scratch->scan));
        esp_err_t collect=wifi_radio_scan_collect(&s_scan_scratch->scan);
        ESP_LOGI(TAG,"AUTO scan #%"PRIu32" cancellation complete status=%u"
                 " collect=%s",e->scan_id,e->scan_success?1U:0U,
                 esp_err_to_name(collect));
        wifi_scan_lifecycle_complete(&s_scan);
        run_foreground_after_barrier();
        return;
    }
    clear_deadline();
    wifi_scan_data_t *scan=&s_scan_scratch->scan;
    memset(scan,0,sizeof(*scan));
    esp_err_t err=e->scan_success?wifi_radio_scan_collect(scan):ESP_FAIL;
    if(!e->scan_success)(void)wifi_radio_scan_collect(scan);
    stack_checkpoint("scan_collected");
    bool automatic=s_scan.owner==WIFI_SCAN_OWNER_AUTO;
    wifi_scan_lifecycle_complete(&s_scan);
    if(automatic&&!auto_allowed()){
        ESP_LOGI(TAG,"AUTO scan completion discarded: foreground owns radio");
        set_state(WIFI_WORKER_STATE_IDLE);
        return;
    }
    if(!automatic){
        if(err==ESP_OK&&xSemaphoreTake(s_mutex,portMAX_DELAY)==pdTRUE){
            s_scan_snapshot.scan=*scan;
            s_scan_snapshot.generation++;
            s_scan_snapshot_tick=xTaskGetTickCount();
            xSemaphoreGive(s_mutex);
        }
        set_state(atomic_load(&s_connected)?WIFI_WORKER_STATE_CONNECTED:WIFI_WORKER_STATE_IDLE);
        result(err==ESP_OK?WIFI_WORKER_RESULT_SCAN_READY:WIFI_WORKER_RESULT_ERROR,
               &s_active_scan_command,err,0,WIFI_CONN_FAIL_NONE,0);
        memset(&s_active_scan_command,0,sizeof(s_active_scan_command));
        return;
    }
    if(err!=ESP_OK){auto_no_candidate();return;}
    wifi_known_visible_ap_t *v=s_scan_scratch->visible;
    for(uint16_t i=0;i<scan->count;i++)v[i]=(wifi_known_visible_ap_t){
        .ssid=scan->records[i].ssid,.bssid=scan->records[i].bssid,
        .rssi=scan->records[i].rssi,.channel=scan->records[i].primary_channel,
        .security=(uint8_t)scan->records[i].security};
    size_t vi=0;
    int ki=wifi_known_db_select_visible(&s_known,v,scan->count,&vi);
    if(ki<0){auto_no_candidate();return;}
    wifi_known_network_t*k=&s_known.entries[ki];
    wifi_worker_cmd_t cmd={.type=WIFI_WORKER_CMD_CONNECT};
    strcpy(cmd.data.connect.ssid,k->ssid);
    strcpy(cmd.data.connect.password,k->password);
    memcpy(cmd.data.connect.bssid,scan->records[vi].bssid,6);
    cmd.data.connect.channel=scan->records[vi].primary_channel;
    cmd.data.connect.security=scan->records[vi].security;
    cmd.data.connect.bssid_lock=false;
    cmd.data.connect.candidate_known=true;
    cmd.data.connect.candidate_rssi=scan->records[vi].rssi;
    cmd.data.connect.candidate_age_ms=0;
    ESP_LOGI(TAG,"AUTO candidate ssid='%s' rssi=%d source=known_network",
             k->ssid,scan->records[vi].rssi);
    set_state(WIFI_WORKER_STATE_IDLE);
    stack_checkpoint("auto_candidate_selected");
    (void)start_attempt(&cmd,WIFI_CONN_ORIGIN_AUTO);
    memset(&cmd,0,sizeof(cmd));
}
static void fail_active(wifi_conn_failure_t f,uint16_t reason,uint32_t elapsed_ms){bool manual=s_active_manual;uint32_t id=s_active_attempt;wifi_worker_cmd_t cmd=s_active_command;(void)wifi_conn_sm_failed(&s_sm,id,f,reason,false);ESP_LOGW(TAG,"CONN #%"PRIu32" DISCONNECTED elapsed=%"PRIu32"ms reason=%u(%s) class=%s",id,elapsed_ms,(unsigned)reason,wifi_disconnect_reason_name(reason),failure_name(f));atomic_store(&s_connected,false);publish_connection(false);clear_deadline();s_active_attempt=0;s_active_manual=false;memset(&s_active_command,0,sizeof(s_active_command));if(manual){set_state(WIFI_WORKER_STATE_IDLE);result(WIFI_WORKER_RESULT_ERROR,&cmd,ESP_FAIL,reason,f,id);}else if(auto_allowed()&&s_sm.phase==WIFI_CONN_PHASE_RETRY_WAIT)schedule_retry();else set_state(WIFI_WORKER_STATE_IDLE);memset(&cmd,0,sizeof(cmd));}
static void handle_connection(const wifi_radio_connection_event_t*e){if(e->attempt_id==0||e->attempt_id!=s_active_attempt){s_sm.counters.stale_events_ignored++;ESP_LOGW(TAG,"stale conn event attempt=%"PRIu32" active=%"PRIu32,e->attempt_id,s_active_attempt);return;}wifi_worker_state_t event_state=state();bool bad_lifecycle=(e->type==WIFI_RADIO_CONN_ASSOCIATED&&event_state!=WIFI_WORKER_STATE_ASSOCIATING)||(e->type==WIFI_RADIO_CONN_GOT_IP&&event_state!=WIFI_WORKER_STATE_WAITING_IP)||(e->type==WIFI_RADIO_CONN_LOST_IP&&event_state!=WIFI_WORKER_STATE_CONNECTED)||(e->type==WIFI_RADIO_CONN_DISCONNECTED&&event_state!=WIFI_WORKER_STATE_ASSOCIATING&&event_state!=WIFI_WORKER_STATE_WAITING_IP&&event_state!=WIFI_WORKER_STATE_CONNECTED&&event_state!=WIFI_WORKER_STATE_SWITCHING_NETWORK&&event_state!=WIFI_WORKER_STATE_DISCONNECTING);if(bad_lifecycle){s_sm.counters.lifecycle_events_rejected++;ESP_LOGW(TAG,"rejected lifecycle event=%u state=%s attempt=%"PRIu32,(unsigned)e->type,phase_name(event_state),e->attempt_id);return;}if(e->type==WIFI_RADIO_CONN_ASSOCIATED&&(state()==WIFI_WORKER_STATE_ASSOCIATING)){memcpy(s_associated_bssid,e->bssid,sizeof(s_associated_bssid));s_associated_channel=e->channel;if(wifi_conn_sm_associated(&s_sm,e->attempt_id)){ESP_LOGI(TAG,"CONN #%"PRIu32" ASSOCIATING -> WAITING_IP",e->attempt_id);set_state(WIFI_WORKER_STATE_WAITING_IP);deadline(DHCP_TIMEOUT_MS);}return;}if(e->type==WIFI_RADIO_CONN_GOT_IP&&state()==WIFI_WORKER_STATE_WAITING_IP){if(!wifi_conn_sm_got_ip(&s_sm,e->attempt_id))return;clear_deadline();atomic_store(&s_connected,true);publish_connection(true);set_state(WIFI_WORKER_STATE_CONNECTED);s_user_suppressed=false;ESP_LOGI(TAG,"CONN #%"PRIu32" GOT_IP -> CONNECTED",e->attempt_id);if(s_active_manual){save_success(&s_active_command);result(WIFI_WORKER_RESULT_CONNECTED,&s_active_command,ESP_OK,0,WIFI_CONN_FAIL_NONE,e->attempt_id);}else{int i=wifi_known_db_find(&s_known,s_connection_snapshot.info.ssid);if(i>=0)(void)wifi_credentials_upsert_success(&s_known,s_known.entries[i].ssid,s_known.entries[i].password,(wifi_security_t)s_known.entries[i].security,s_associated_channel,s_associated_bssid);}s_active_manual=false;memset(&s_active_command,0,sizeof(s_active_command));return;}if(e->type==WIFI_RADIO_CONN_DISCONNECTED){if(state()==WIFI_WORKER_STATE_DISCONNECTING&&s_foreground_barrier==FOREGROUND_BARRIER_DISCONNECT){ESP_LOGI(TAG,"AUTO connect #%"PRIu32" cancellation complete reason=%u",e->attempt_id,(unsigned)e->disconnect_reason);atomic_store(&s_connected,false);publish_connection(false);s_active_attempt=0;s_active_manual=false;s_internal_disconnect=false;wifi_conn_sm_user_disconnect(&s_sm);memset(&s_active_command,0,sizeof(s_active_command));run_foreground_after_barrier();return;}if(state()==WIFI_WORKER_STATE_SWITCHING_NETWORK&&s_switch_pending){atomic_store(&s_connected,false);publish_connection(false);clear_deadline();wifi_worker_cmd_t target=s_switch_target;s_switch_pending=false;memset(&s_switch_target,0,sizeof(s_switch_target));set_state(WIFI_WORKER_STATE_IDLE);s_active_attempt=0;(void)start_attempt(&target,WIFI_CONN_ORIGIN_MANUAL);memset(&target,0,sizeof(target));return;}if(state()==WIFI_WORKER_STATE_DISCONNECTING){atomic_store(&s_connected,false);publish_connection(false);clear_deadline();s_active_attempt=0;set_state(WIFI_WORKER_STATE_IDLE);wifi_conn_sm_user_disconnect(&s_sm);if(s_internal_disconnect){s_internal_disconnect=false;if(auto_allowed()){s_sm.phase=WIFI_CONN_PHASE_RETRY_WAIT;s_sm.retry_count=1;schedule_retry();}}else result(s_active_command.type==WIFI_WORKER_CMD_FORGET_NETWORK?WIFI_WORKER_RESULT_NETWORK_FORGOTTEN:WIFI_WORKER_RESULT_DISCONNECTED,&s_active_command,ESP_OK,e->disconnect_reason,WIFI_CONN_FAIL_USER,e->attempt_id);memset(&s_active_command,0,sizeof(s_active_command));return;}if(state()==WIFI_WORKER_STATE_CONNECTED){s_sm.origin=WIFI_CONN_ORIGIN_AUTO;s_sm.auto_enabled=any_auto();(void)wifi_conn_sm_failed(&s_sm,e->attempt_id,WIFI_CONN_FAIL_TRANSIENT,e->disconnect_reason,true);atomic_store(&s_connected,false);publish_connection(false);s_active_attempt=0;if(auto_allowed())schedule_retry();else set_state(WIFI_WORKER_STATE_IDLE);return;}fail_active(classify(e->disconnect_reason),e->disconnect_reason,e->elapsed_ms);return;}if(e->type==WIFI_RADIO_CONN_LOST_IP&&state()==WIFI_WORKER_STATE_CONNECTED){ESP_LOGW(TAG,"CONN #%"PRIu32" LOST_IP",e->attempt_id);atomic_store(&s_connected,false);publish_connection(false);s_internal_disconnect=true;s_active_command=(wifi_worker_cmd_t){0};if(wifi_radio_disconnect()==ESP_OK){set_state(WIFI_WORKER_STATE_DISCONNECTING);deadline(DISCONNECT_TIMEOUT_MS);}else{set_state(WIFI_WORKER_STATE_IDLE);if(auto_allowed()){s_sm.phase=WIFI_CONN_PHASE_RETRY_WAIT;s_sm.retry_count=1;schedule_retry();}}}}
static void preempt_for_manual(const wifi_worker_cmd_t*cmd){if(s_active_manual&&s_active_command.request_id)result(WIFI_WORKER_RESULT_ERROR,&s_active_command,ESP_ERR_INVALID_STATE,0,WIFI_CONN_FAIL_USER,s_active_attempt);s_switch_target=*cmd;s_switch_pending=true;s_active_manual=false;if(wifi_radio_disconnect()==ESP_OK){set_state(WIFI_WORKER_STATE_SWITCHING_NETWORK);deadline(DISCONNECT_TIMEOUT_MS);}else{wifi_worker_cmd_t target=s_switch_target;s_switch_pending=false;(void)wifi_radio_stop();set_state(WIFI_WORKER_STATE_OFF);s_active_attempt=0;if(ensure_radio()==ESP_OK)(void)start_attempt(&target,WIFI_CONN_ORIGIN_MANUAL);else result(WIFI_WORKER_RESULT_ERROR,&target,ESP_FAIL,0,WIFI_CONN_FAIL_RADIO,0);memset(&target,0,sizeof(target));}}
static esp_err_t preempt_auto_scan_for_foreground(const char *operation,
                                                   uint32_t request_id)
{
    if (state() != WIFI_WORKER_STATE_SCANNING ||
        s_scan.owner != WIFI_SCAN_OWNER_AUTO) return ESP_OK;
    uint32_t retired = s_scan.active_id;
    ESP_LOGI(TAG, "AUTO scan #%" PRIu32 " preempted by %s request=%" PRIu32,
             retired, operation, request_id);
    (void)wifi_radio_scan_cancel();
    clear_deadline();
    (void)wifi_scan_lifecycle_preempt_auto(&s_scan, NULL);
    esp_err_t err = wifi_radio_stop();
    set_state(err == ESP_OK ? WIFI_WORKER_STATE_OFF : WIFI_WORKER_STATE_ERROR);
    if (err != ESP_OK) return err;
    return ensure_radio();
}
static void command(const wifi_worker_cmd_t*c){esp_err_t e=ESP_OK;log_app_action_during_connect(c);if(c->type==WIFI_WORKER_CMD_ENABLE_PROMISCUOUS)atomic_store(&s_auto_suspended,true);if(c->type==WIFI_WORKER_CMD_CONNECT||c->type==WIFI_WORKER_CMD_DISCONNECT)s_user_suppressed=true;if(defer_foreground_if_needed(c))return;switch(c->type){case WIFI_WORKER_CMD_START:e=ensure_radio();if(e==ESP_OK){result(WIFI_WORKER_RESULT_STARTED,c,ESP_OK,0,WIFI_CONN_FAIL_NONE,0);return;}break;case WIFI_WORKER_CMD_SCAN:{
    if(ensure_radio()!=ESP_OK){e=ESP_FAIL;break;}
    e=preempt_auto_scan_for_foreground("USER scan",c->request_id);
    if(e!=ESP_OK)break;
    if(state()!=WIFI_WORKER_STATE_IDLE&&state()!=WIFI_WORKER_STATE_CONNECTED){
        e=ESP_ERR_INVALID_STATE;break;
    }
    uint32_t id=(uint32_t)atomic_fetch_add(&s_next_scan,1);
    if(id==0)id=(uint32_t)atomic_fetch_add(&s_next_scan,1);
    s_active_scan_command=*c;
    e=wifi_radio_scan_start(&c->data.scan,id);
    if(e==ESP_OK){
        (void)wifi_scan_lifecycle_begin(&s_scan,id,WIFI_SCAN_OWNER_USER);
        set_state(WIFI_WORKER_STATE_SCANNING);
        deadline(SCAN_TIMEOUT_MS);
        ESP_LOGI(TAG,"USER scan #%"PRIu32" started request=%"PRIu32,id,c->request_id);
        return;
    }
    memset(&s_active_scan_command,0,sizeof(s_active_scan_command));
    break;
}case WIFI_WORKER_CMD_CONNECT:s_user_suppressed=true;if(ensure_radio()!=ESP_OK){e=ESP_FAIL;break;}if(state()==WIFI_WORKER_STATE_SCANNING){wifi_scan_owner_t owner=s_scan.owner;(void)wifi_radio_scan_cancel();clear_deadline();if(owner==WIFI_SCAN_OWNER_USER&&s_active_scan_command.request_id)result(WIFI_WORKER_RESULT_ERROR,&s_active_scan_command,ESP_ERR_INVALID_STATE,0,WIFI_CONN_FAIL_USER,0);wifi_scan_lifecycle_complete(&s_scan);memset(&s_active_scan_command,0,sizeof(s_active_scan_command));(void)wifi_radio_stop();set_state(WIFI_WORKER_STATE_OFF);if(ensure_radio()!=ESP_OK){e=ESP_FAIL;break;}}if(state()==WIFI_WORKER_STATE_IDLE||state()==WIFI_WORKER_STATE_RETRY_WAIT){e=start_attempt(c,WIFI_CONN_ORIGIN_MANUAL);if(e==ESP_OK)return;break;}if(state()==WIFI_WORKER_STATE_CONNECTED||state()==WIFI_WORKER_STATE_ASSOCIATING||state()==WIFI_WORKER_STATE_WAITING_IP){preempt_for_manual(c);return;}e=ESP_ERR_INVALID_STATE;break;case WIFI_WORKER_CMD_DISCONNECT:s_user_suppressed=true;e=preempt_auto_scan_for_foreground("USER disconnect",c->request_id);if(e!=ESP_OK)break;if(state()==WIFI_WORKER_STATE_IDLE||state()==WIFI_WORKER_STATE_RETRY_WAIT){atomic_store(&s_connected,false);publish_connection(false);result(WIFI_WORKER_RESULT_DISCONNECTED,c,ESP_OK,0,WIFI_CONN_FAIL_USER,0);return;}if(state()!=WIFI_WORKER_STATE_CONNECTED){e=ESP_ERR_INVALID_STATE;break;}s_active_command=*c;s_internal_disconnect=false;e=wifi_radio_disconnect();if(e==ESP_OK){set_state(WIFI_WORKER_STATE_DISCONNECTING);deadline(DISCONNECT_TIMEOUT_MS);return;}break;case WIFI_WORKER_CMD_SET_AUTO_CONNECT:{e=wifi_credentials_set_network_auto(&s_known,c->data.auto_connect.ssid,c->data.auto_connect.enabled);if(e!=ESP_OK)break;wifi_conn_sm_set_auto(&s_sm,any_auto());if(c->data.auto_connect.enabled){s_user_suppressed=false;atomic_store(&s_auto_kick,true);}ESP_LOGI(TAG,"AUTO policy ssid='%s' enabled=%u; active manual attempt unchanged",c->data.auto_connect.ssid,c->data.auto_connect.enabled?1U:0U);result(WIFI_WORKER_RESULT_AUTO_CONNECT_UPDATED,c,ESP_OK,0,WIFI_CONN_FAIL_NONE,s_active_attempt);return;}case WIFI_WORKER_CMD_FORGET_NETWORK:{bool current=s_connection_snapshot.info.connected&&!strcmp(s_connection_snapshot.info.ssid,c->data.forget.ssid);e=wifi_credentials_forget_network(&s_known,c->data.forget.ssid);if(e!=ESP_OK)break;wifi_conn_sm_set_auto(&s_sm,any_auto());if(current&&state()==WIFI_WORKER_STATE_CONNECTED){s_active_command=*c;s_user_suppressed=true;e=wifi_radio_disconnect();if(e==ESP_OK){set_state(WIFI_WORKER_STATE_DISCONNECTING);deadline(DISCONNECT_TIMEOUT_MS);return;}}result(WIFI_WORKER_RESULT_NETWORK_FORGOTTEN,c,ESP_OK,0,WIFI_CONN_FAIL_NONE,0);return;}case WIFI_WORKER_CMD_ENABLE_PROMISCUOUS:atomic_store(&s_auto_suspended,true);e=preempt_auto_scan_for_foreground("PROMISCUOUS",c->request_id);if(e!=ESP_OK)break;if(state()!=WIFI_WORKER_STATE_IDLE){e=ESP_ERR_INVALID_STATE;break;}if(c->data.promiscuous.callback==wifi_promiscuous_rx_cb){e=wifi_promiscuous_init();if(e==ESP_OK)e=wifi_analyzer_reset();if(e!=ESP_OK)break;}e=wifi_radio_set_mode_promiscuous(c->data.promiscuous.channel,c->data.promiscuous.callback);if(e==ESP_OK){wifi_channel_engine_worker_begin_fixed(c->data.promiscuous.channel);set_state(WIFI_WORKER_STATE_PROMISCUOUS);result(WIFI_WORKER_RESULT_PROMISCUOUS_ENABLED,c,ESP_OK,0,WIFI_CONN_FAIL_NONE,0);return;}break;case WIFI_WORKER_CMD_DISABLE_PROMISCUOUS:if(state()!=WIFI_WORKER_STATE_PROMISCUOUS){e=ESP_ERR_INVALID_STATE;break;}wifi_channel_hopper_worker_stop();e=wifi_radio_disable_promiscuous();if(e==ESP_OK){set_state(WIFI_WORKER_STATE_IDLE);result(WIFI_WORKER_RESULT_PROMISCUOUS_DISABLED,c,ESP_OK,0,WIFI_CONN_FAIL_NONE,0);return;}break;case WIFI_WORKER_CMD_HOPPER_START:if(state()!=WIFI_WORKER_STATE_PROMISCUOUS){e=ESP_ERR_INVALID_STATE;break;}e=wifi_channel_hopper_worker_start(c->data.hopper_interval_ms);if(e==ESP_OK){result(WIFI_WORKER_RESULT_HOPPER_STARTED,c,ESP_OK,0,WIFI_CONN_FAIL_NONE,0);return;}break;case WIFI_WORKER_CMD_HOPPER_STOP:wifi_channel_hopper_worker_stop();result(WIFI_WORKER_RESULT_HOPPER_STOPPED,c,ESP_OK,0,WIFI_CONN_FAIL_NONE,0);return;case WIFI_WORKER_CMD_SET_CHANNEL:if(state()!=WIFI_WORKER_STATE_PROMISCUOUS){e=ESP_ERR_INVALID_STATE;break;}{uint8_t first,last;e=wifi_radio_get_channel_range(&first,&last);if(e!=ESP_OK)break;wifi_worker_cmd_t a=*c;if(a.data.channel<first)a.data.channel=last;else if(a.data.channel>last)a.data.channel=first;wifi_channel_hopper_worker_stop();e=wifi_radio_promiscuous_set_channel(a.data.channel);if(e==ESP_OK){wifi_channel_engine_worker_set_fixed(a.data.channel);result(WIFI_WORKER_RESULT_CHANNEL_SET,&a,ESP_OK,0,WIFI_CONN_FAIL_NONE,0);return;}}break;case WIFI_WORKER_CMD_STOP:wifi_channel_hopper_worker_stop();set_state(WIFI_WORKER_STATE_STOPPING);e=wifi_radio_stop();atomic_store(&s_connected,false);publish_connection(false);set_state(e==ESP_OK?WIFI_WORKER_STATE_OFF:WIFI_WORKER_STATE_ERROR);if(e==ESP_OK){result(WIFI_WORKER_RESULT_STOPPED,c,ESP_OK,0,WIFI_CONN_FAIL_NONE,0);return;}break;default:e=ESP_ERR_INVALID_ARG;break;}ESP_LOGW(TAG,"command=%d failed: %s",c->type,esp_err_to_name(e));result(WIFI_WORKER_RESULT_ERROR,c,e,0,WIFI_CONN_FAIL_NONE,s_active_attempt);}
static void timeout(void){wifi_worker_state_t st=state();ESP_LOGE(TAG,"timeout state=%s attempt=%"PRIu32" foreground_barrier=%u",phase_name(st),s_active_attempt,(unsigned)s_foreground_barrier);clear_deadline();if(s_foreground_barrier!=FOREGROUND_BARRIER_NONE){foreground_barrier_t barrier=s_foreground_barrier;esp_err_t stop_err=wifi_radio_stop();ESP_LOGW(TAG,"foreground barrier timeout type=%u radio_stop=%s",(unsigned)barrier,esp_err_to_name(stop_err));if(barrier==FOREGROUND_BARRIER_SCAN)wifi_scan_lifecycle_complete(&s_scan);s_active_attempt=0;s_active_manual=false;s_internal_disconnect=false;wifi_conn_sm_user_disconnect(&s_sm);set_state(stop_err==ESP_OK?WIFI_WORKER_STATE_OFF:WIFI_WORKER_STATE_ERROR);if(stop_err==ESP_OK&&ensure_radio()==ESP_OK){run_foreground_after_barrier();}else{if(s_foreground_pending)result(WIFI_WORKER_RESULT_ERROR,&s_foreground_command,ESP_ERR_TIMEOUT,0,WIFI_CONN_FAIL_RADIO,0);s_foreground_pending=false;s_foreground_barrier=FOREGROUND_BARRIER_NONE;memset(&s_foreground_command,0,sizeof(s_foreground_command));}return;}if(st==WIFI_WORKER_STATE_SCANNING){if(!wifi_scan_lifecycle_deadline_is_current(&s_scan)){ESP_LOGW(TAG,"ignored stale scan deadline active=%"PRIu32" deadline=%"PRIu32,s_scan.active_id,s_scan.deadline_id);return;}(void)wifi_radio_scan_cancel();radio_event_t e={.kind=RADIO_SCAN_DONE,.scan_id=s_scan.active_id,.scan_success=false};finish_scan(&e);return;}if(st==WIFI_WORKER_STATE_ASSOCIATING||st==WIFI_WORKER_STATE_WAITING_IP){bool manual=s_active_manual;wifi_worker_cmd_t cmd=s_active_command;wifi_conn_failure_t f=st==WIFI_WORKER_STATE_WAITING_IP?WIFI_CONN_FAIL_IP_TIMEOUT:WIFI_CONN_FAIL_TRANSIENT;(void)wifi_conn_sm_timeout(&s_sm,s_active_attempt);uint32_t id=s_active_attempt;(void)wifi_radio_stop();set_state(WIFI_WORKER_STATE_OFF);(void)ensure_radio();s_active_attempt=0;s_active_manual=false;memset(&s_active_command,0,sizeof(s_active_command));if(manual)result(WIFI_WORKER_RESULT_ERROR,&cmd,ESP_ERR_TIMEOUT,0,f,id);else if(auto_allowed())schedule_retry();return;}if(st==WIFI_WORKER_STATE_SWITCHING_NETWORK||st==WIFI_WORKER_STATE_DISCONNECTING){(void)wifi_radio_stop();set_state(WIFI_WORKER_STATE_OFF);(void)ensure_radio();if(s_switch_pending){wifi_worker_cmd_t target=s_switch_target;s_switch_pending=false;(void)start_attempt(&target,WIFI_CONN_ORIGIN_MANUAL);}else if(s_active_command.request_id)result(WIFI_WORKER_RESULT_ERROR,&s_active_command,ESP_ERR_TIMEOUT,0,WIFI_CONN_FAIL_RADIO,s_active_attempt);}}
static void worker(void*arg){(void)arg;ESP_LOGI(TAG,"Connection Manager started state=%s known=%u auto=%u",phase_name(state()),(unsigned)wifi_known_db_count(&s_known),(unsigned)wifi_known_db_auto_count(&s_known));stack_checkpoint("task_entry");if(any_auto())atomic_store(&s_auto_kick,true);for(;;){if(atomic_exchange(&s_auto_resume_requested,false)){s_user_suppressed=false;atomic_store(&s_auto_kick,true);ESP_LOGI(TAG,"AUTO ownership resumed by foreground release");}publish_policy_snapshots();if(state()==WIFI_WORKER_STATE_PROMISCUOUS)wifi_channel_hopper_worker_tick();else wifi_channel_hopper_worker_stop();radio_event_t re;while(xQueueReceive(s_radio_events,&re,0)==pdTRUE){if(re.kind==RADIO_SCAN_DONE)finish_scan(&re);else handle_connection(&re.connection);}if(s_deadline&&reached(xTaskGetTickCount(),s_deadline))timeout();wifi_worker_cmd_t c;if(xQueueReceive(s_commands,&c,pdMS_TO_TICKS(25))==pdTRUE){command(&c);memset(&c,0,sizeof(c));continue;}if(atomic_load(&s_auto_suspended)){if(s_foreground_barrier==FOREGROUND_BARRIER_NONE&&s_scan.owner==WIFI_SCAN_OWNER_AUTO){esp_err_t cancel_err=wifi_radio_scan_cancel();ESP_LOGI(TAG,"AUTO scan #%"PRIu32" suspend cancel driver=%s",s_scan.active_id,esp_err_to_name(cancel_err));if(cancel_err==ESP_OK){s_foreground_barrier=FOREGROUND_BARRIER_SCAN;deadline(DISCONNECT_TIMEOUT_MS);}}else if(s_foreground_barrier==FOREGROUND_BARRIER_NONE&&!s_active_manual&&(state()==WIFI_WORKER_STATE_ASSOCIATING||state()==WIFI_WORKER_STATE_WAITING_IP)){esp_err_t disconnect_err=wifi_radio_disconnect();ESP_LOGI(TAG,"AUTO connect #%"PRIu32" suspend disconnect driver=%s",s_active_attempt,esp_err_to_name(disconnect_err));if(disconnect_err==ESP_OK){s_foreground_barrier=FOREGROUND_BARRIER_DISCONNECT;set_state(WIFI_WORKER_STATE_DISCONNECTING);deadline(DISCONNECT_TIMEOUT_MS);}}continue;}bool kick=atomic_exchange(&s_auto_kick,false);if(kick&&(state()==WIFI_WORKER_STATE_OFF||state()==WIFI_WORKER_STATE_ERROR||state()==WIFI_WORKER_STATE_RETRY_WAIT))s_retry_at=0;bool retry_due=reached(xTaskGetTickCount(),s_retry_at);if(wifi_conn_auto_activation_due(auto_allowed(),auto_runtime(),retry_due))auto_scan_start();}}
esp_err_t wifi_worker_init(void){if(s_task)return ESP_OK;wifi_scan_lifecycle_init(&s_scan);wifi_known_db_init(&s_known);esp_err_t load=wifi_credentials_load_database(&s_known);if(load==ESP_ERR_NVS_NOT_FOUND)ESP_LOGI(TAG,"Known Networks NVS: missing (empty database)");else if(load!=ESP_OK){ESP_LOGE(TAG,"Known Networks NVS corrupt/unavailable: %s",esp_err_to_name(load));wifi_known_db_init(&s_known);}ESP_LOGI(TAG,"Known Networks loaded: count=%u auto=%u",(unsigned)wifi_known_db_count(&s_known),(unsigned)wifi_known_db_auto_count(&s_known));for(unsigned i=0;i<WIFI_KNOWN_NETWORK_MAX;i++)if(s_known.entries[i].occupied)ESP_LOGI(TAG,"known ssid='%s' auto=%u security=%s",s_known.entries[i].ssid,s_known.entries[i].auto_connect?1U:0U,wifi_security_name((wifi_security_t)s_known.entries[i].security));wifi_conn_sm_init(&s_sm,any_auto());s_known_snapshot=s_known;s_sm_snapshot=s_sm;s_commands=xQueueCreate(CMD_QUEUE_LEN,sizeof(wifi_worker_cmd_t));s_results=xQueueCreate(RESULT_QUEUE_LEN,sizeof(wifi_worker_result_t));s_radio_events=xQueueCreate(RADIO_QUEUE_LEN,sizeof(radio_event_t));s_mutex=xSemaphoreCreateMutex();s_scan_scratch=heap_caps_calloc(1,sizeof(*s_scan_scratch),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!s_commands||!s_results||!s_radio_events||!s_mutex||!s_scan_scratch)goto fail;ESP_LOGI(TAG,"scan scratch=%u bytes in PSRAM",(unsigned)sizeof(*s_scan_scratch));if(xTaskCreate(worker,"wifi_conn_mgr",WORKER_STACK,NULL,WORKER_PRIORITY,&s_task)!=pdPASS)goto fail;return ESP_OK;fail:if(s_commands){vQueueDelete(s_commands);s_commands=NULL;}if(s_results){vQueueDelete(s_results);s_results=NULL;}if(s_radio_events){vQueueDelete(s_radio_events);s_radio_events=NULL;}if(s_mutex){vSemaphoreDelete(s_mutex);s_mutex=NULL;}if(s_scan_scratch){heap_caps_free(s_scan_scratch);s_scan_scratch=NULL;}return ESP_ERR_NO_MEM;}
static esp_err_t send(wifi_worker_cmd_t*c,uint32_t*id){if(!c||!id||!s_commands)return ESP_ERR_INVALID_ARG;c->request_id=(uint32_t)atomic_fetch_add(&s_next_request,1);atomic_fetch_add(&s_pending_requests,1);if(xQueueSend(s_commands,c,0)!=pdTRUE){pending_done();return ESP_ERR_TIMEOUT;}*id=c->request_id;return ESP_OK;}
esp_err_t wifi_worker_send_start(uint32_t*i){wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_START};return send(&c,i);}esp_err_t wifi_worker_send_scan(const wifi_scan_options_t*o,uint32_t*i){if(!o)return ESP_ERR_INVALID_ARG;wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_SCAN,.data.scan=*o};return send(&c,i);}esp_err_t wifi_worker_send_stop(uint32_t*i){wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_STOP};return send(&c,i);}esp_err_t wifi_worker_send_hopper_start(uint32_t ms,uint32_t*i){if(ms&&(ms<50||ms>60000))return ESP_ERR_INVALID_ARG;wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_HOPPER_START,.data.hopper_interval_ms=ms};return send(&c,i);}esp_err_t wifi_worker_send_hopper_stop(uint32_t*i){wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_HOPPER_STOP};return send(&c,i);}esp_err_t wifi_worker_send_set_channel(uint8_t ch,uint32_t*i){if(!ch||ch>14)return ESP_ERR_INVALID_ARG;wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_SET_CHANNEL,.data.channel=ch};return send(&c,i);}esp_err_t wifi_worker_send_disconnect(uint32_t*i){wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_DISCONNECT};return send(&c,i);}esp_err_t wifi_worker_send_enable_promiscuous(uint8_t ch,wifi_promiscuous_cb_t cb,uint32_t*i){if(!ch||ch>14||!cb)return ESP_ERR_INVALID_ARG;wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_ENABLE_PROMISCUOUS,.data.promiscuous={ch,cb}};return send(&c,i);}esp_err_t wifi_worker_send_disable_promiscuous(uint32_t*i){wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_DISABLE_PROMISCUOUS};return send(&c,i);}
esp_err_t wifi_worker_send_connect(const char*ssid,const char*password,const uint8_t bssid[6],uint32_t*i){if(!ssid||!password||!bssid||!i)return ESP_ERR_INVALID_ARG;size_t sn=strnlen(ssid,33),pn=strnlen(password,65);if(!sn||sn>32||pn>64)return ESP_ERR_INVALID_ARG;wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_CONNECT};memcpy(c.data.connect.ssid,ssid,sn);memcpy(c.data.connect.password,password,pn);memcpy(c.data.connect.bssid,bssid,6);c.data.connect.bssid_lock=false;if(s_mutex&&xSemaphoreTake(s_mutex,pdMS_TO_TICKS(5))==pdTRUE){for(uint16_t n=0;n<s_scan_snapshot.scan.count;n++)if(!memcmp(s_scan_snapshot.scan.records[n].bssid,bssid,6)){c.data.connect.channel=s_scan_snapshot.scan.records[n].primary_channel;c.data.connect.security=s_scan_snapshot.scan.records[n].security;c.data.connect.candidate_rssi=s_scan_snapshot.scan.records[n].rssi;c.data.connect.candidate_known=true;c.data.connect.candidate_age_ms=(uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()-s_scan_snapshot_tick);break;}xSemaphoreGive(s_mutex);}return send(&c,i);}
bool wifi_worker_get_saved_network_state(const char*ssid,bool*enabled){if(!ssid||!enabled||!s_mutex)return false;if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(5))!=pdTRUE)return false;int i=wifi_known_db_find(&s_known_snapshot,ssid);*enabled=i>=0&&s_known_snapshot.entries[i].auto_connect;xSemaphoreGive(s_mutex);return i>=0;}bool wifi_worker_has_saved_network(const char*s){bool e=false;return wifi_worker_get_saved_network_state(s,&e);}esp_err_t wifi_worker_send_set_auto_connect(const char*s,bool e,uint32_t*i){if(!s||!i)return ESP_ERR_INVALID_ARG;size_t n=strnlen(s,33);if(!n||n>32)return ESP_ERR_INVALID_ARG;wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_SET_AUTO_CONNECT,.data.auto_connect.enabled=e};memcpy(c.data.auto_connect.ssid,s,n);return send(&c,i);}esp_err_t wifi_worker_send_forget_network(const char*s,uint32_t*i){if(!s||!i)return ESP_ERR_INVALID_ARG;size_t n=strnlen(s,33);if(!n||n>32)return ESP_ERR_INVALID_ARG;wifi_worker_cmd_t c={.type=WIFI_WORKER_CMD_FORGET_NETWORK};memcpy(c.data.forget.ssid,s,n);return send(&c,i);}esp_err_t wifi_worker_send_connect_saved(const char*s,const uint8_t b[6],uint32_t*i){if(!s||!b||!i||!s_mutex)return ESP_ERR_INVALID_ARG;wifi_known_network_t k={0};if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(5))!=pdTRUE)return ESP_ERR_TIMEOUT;int n=wifi_known_db_find(&s_known_snapshot,s);if(n>=0)k=s_known_snapshot.entries[n];xSemaphoreGive(s_mutex);if(n<0)return ESP_ERR_NOT_FOUND;esp_err_t e=wifi_worker_send_connect(k.ssid,k.password,b,i);memset(&k,0,sizeof(k));return e;}
bool wifi_worker_receive_result(wifi_worker_result_t*r){return r&&s_results&&xQueueReceive(s_results,r,0)==pdTRUE;}bool wifi_worker_get_scan_snapshot(wifi_scan_snapshot_t*out){if(!out||!s_mutex||xSemaphoreTake(s_mutex,0)!=pdTRUE)return false;*out=s_scan_snapshot;xSemaphoreGive(s_mutex);return true;}bool wifi_worker_get_connection_snapshot(wifi_connection_snapshot_t*out){if(!out||!s_mutex||xSemaphoreTake(s_mutex,0)!=pdTRUE)return false;*out=s_connection_snapshot;xSemaphoreGive(s_mutex);return true;}wifi_worker_state_t wifi_worker_get_state(void){return state();}bool wifi_worker_request_pending(void){return atomic_load(&s_pending_requests)!=0;}bool wifi_worker_is_connected(void){return atomic_load(&s_connected);}bool wifi_worker_get_connection_diagnostics(wifi_conn_sm_t*out){if(!out||!s_mutex||xSemaphoreTake(s_mutex,pdMS_TO_TICKS(2))!=pdTRUE)return false;*out=s_sm_snapshot;xSemaphoreGive(s_mutex);return true;}void wifi_worker_suspend_auto_connect(void){atomic_store(&s_auto_resume_requested,false);atomic_store(&s_auto_kick,false);atomic_store(&s_auto_suspended,true);}void wifi_worker_resume_auto_connect(void){atomic_store(&s_auto_suspended,false);atomic_store(&s_auto_resume_requested,true);}bool wifi_worker_auto_connect_is_suspended(void){return atomic_load(&s_auto_suspended);}bool wifi_worker_get_channel_range(uint8_t*f,uint8_t*l){if(!f||!l)return false;uint8_t a=atomic_load(&s_first_channel),b=atomic_load(&s_last_channel);if(a<1||b<a||b>14)return false;*f=a;*l=b;return true;}
