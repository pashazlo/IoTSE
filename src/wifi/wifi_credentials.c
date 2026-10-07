#include "wifi_credentials.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

#define NS "wifi_auth"
#define KEY_DB "known_v3"
#define KEY_LEGACY "last_good"
#define LEGACY_MAGIC 0x57464352UL
#define LEGACY_V1 1U
#define LEGACY_V2 2U
#define LEGACY_AUTO 0x01U
#define LEGACY_INITIALIZED 0x80U
static const char *TAG="WIFI_CREDENTIALS";
typedef struct __attribute__((packed)){uint32_t magic;uint8_t version,ssid_length,password_length,flags;char ssid[33];char password[65];} legacy_record_t;
_Static_assert(sizeof(legacy_record_t)==106U,"legacy layout");

static bool legacy_valid(const legacy_record_t*r){return r&&r->magic==LEGACY_MAGIC&&(r->version==LEGACY_V1||r->version==LEGACY_V2)&&r->ssid_length>0&&r->ssid_length<=32&&r->password_length<=64&&r->ssid[r->ssid_length]=='\0'&&r->password[r->password_length]=='\0'&&strnlen(r->ssid,r->ssid_length+1U)==r->ssid_length&&strnlen(r->password,r->password_length+1U)==r->password_length;}
static esp_err_t save_blob(const wifi_known_network_db_t*db){uint8_t wire[WIFI_KNOWN_DB_ENCODED_MAX];size_t n=wifi_known_db_encode(db,wire,sizeof(wire));if(!n)return ESP_ERR_INVALID_ARG;nvs_handle_t h=0;esp_err_t e=nvs_open(NS,NVS_READWRITE,&h);if(e==ESP_OK)e=nvs_set_blob(h,KEY_DB,wire,n);if(e==ESP_OK)e=nvs_commit(h);if(h)nvs_close(h);memset(wire,0,sizeof(wire));return e;}
esp_err_t wifi_credentials_save_database(const wifi_known_network_db_t*db){return save_blob(db);}
static esp_err_t migrate_legacy(nvs_handle_t h,wifi_known_network_db_t*db){legacy_record_t r={0};size_t n=sizeof(r);esp_err_t e=nvs_get_blob(h,KEY_LEGACY,&r,&n);if(e!=ESP_OK)return e;if(n!=sizeof(r)||!legacy_valid(&r)){memset(&r,0,sizeof(r));return ESP_ERR_INVALID_STATE;}wifi_known_db_init(db);if(!wifi_known_db_upsert_success(db,r.ssid,r.password,WIFI_SECURITY_UNKNOWN,0,NULL,false)){memset(&r,0,sizeof(r));return ESP_ERR_INVALID_STATE;}int i=wifi_known_db_find(db,r.ssid);db->entries[i].auto_connect=r.version==LEGACY_V1||(r.flags&LEGACY_INITIALIZED)==0||(r.flags&LEGACY_AUTO)!=0;memset(&r,0,sizeof(r));e=save_blob(db);if(e==ESP_OK)ESP_LOGI(TAG,"Migrated legacy last_good record to known_v3");return e;}
esp_err_t wifi_credentials_load_database(wifi_known_network_db_t*db){if(!db)return ESP_ERR_INVALID_ARG;wifi_known_db_init(db);nvs_handle_t h=0;esp_err_t e=nvs_open(NS,NVS_READONLY,&h);if(e!=ESP_OK)return e;uint8_t wire[WIFI_KNOWN_DB_ENCODED_MAX];size_t n=sizeof(wire);e=nvs_get_blob(h,KEY_DB,wire,&n);if(e==ESP_OK){wifi_known_codec_status_t s=wifi_known_db_decode(wire,n,db);nvs_close(h);memset(wire,0,sizeof(wire));return s==WIFI_KNOWN_CODEC_OK?ESP_OK:ESP_ERR_INVALID_STATE;}if(e==ESP_ERR_NVS_NOT_FOUND)e=migrate_legacy(h,db);nvs_close(h);memset(wire,0,sizeof(wire));return e;}
esp_err_t wifi_credentials_upsert_success(wifi_known_network_db_t*db,const char*ssid,const char*password,wifi_security_t security,uint8_t channel,const uint8_t bssid[6]){if(!db)return ESP_ERR_INVALID_ARG;wifi_known_network_db_t copy=*db;if(!wifi_known_db_upsert_success(&copy,ssid,password,(uint8_t)security,channel,bssid,true))return ESP_ERR_INVALID_ARG;esp_err_t e=save_blob(&copy);if(e==ESP_OK)*db=copy;memset(&copy,0,sizeof(copy));return e;}
esp_err_t wifi_credentials_set_network_auto(wifi_known_network_db_t*db,const char*ssid,bool enabled){if(!db||!ssid)return ESP_ERR_INVALID_ARG;wifi_known_network_db_t copy=*db;if(!wifi_known_db_set_auto(&copy,ssid,enabled))return ESP_ERR_NOT_FOUND;esp_err_t e=save_blob(&copy);if(e==ESP_OK)*db=copy;memset(&copy,0,sizeof(copy));return e;}
esp_err_t wifi_credentials_forget_network(wifi_known_network_db_t*db,const char*ssid){if(!db||!ssid)return ESP_ERR_INVALID_ARG;wifi_known_network_db_t copy=*db;if(!wifi_known_db_forget(&copy,ssid))return ESP_ERR_NOT_FOUND;esp_err_t e=save_blob(&copy);if(e==ESP_OK)*db=copy;memset(&copy,0,sizeof(copy));return e;}
static int newest(const wifi_known_network_db_t*db){int b=-1;for(unsigned i=0;i<WIFI_KNOWN_NETWORK_MAX;i++)if(db->entries[i].occupied&&(b<0||db->entries[i].success_sequence>db->entries[b].success_sequence))b=(int)i;return b;}
esp_err_t wifi_credentials_load(wifi_saved_credentials_t*out){if(!out)return ESP_ERR_INVALID_ARG;memset(out,0,sizeof(*out));wifi_known_network_db_t db;esp_err_t e=wifi_credentials_load_database(&db);if(e!=ESP_OK)return e;int i=newest(&db);if(i<0)return ESP_ERR_NVS_NOT_FOUND;strcpy(out->ssid,db.entries[i].ssid);strcpy(out->password,db.entries[i].password);out->auto_connect=db.entries[i].auto_connect;memset(&db,0,sizeof(db));return ESP_OK;}
esp_err_t wifi_credentials_save(const char*ssid,const char*password,bool auto_connect){wifi_known_network_db_t db;esp_err_t e=wifi_credentials_load_database(&db);if(e==ESP_ERR_NVS_NOT_FOUND)wifi_known_db_init(&db);else if(e!=ESP_OK)return e;if(!wifi_known_db_upsert_success(&db,ssid,password,0,0,NULL,true))return ESP_ERR_INVALID_ARG;int i=wifi_known_db_find(&db,ssid);db.entries[i].auto_connect=auto_connect;e=save_blob(&db);memset(&db,0,sizeof(db));return e;}
esp_err_t wifi_credentials_set_auto_connect(bool enabled){wifi_known_network_db_t db;esp_err_t e=wifi_credentials_load_database(&db);if(e!=ESP_OK)return e;int i=newest(&db);if(i<0)return ESP_ERR_NVS_NOT_FOUND;return wifi_credentials_set_network_auto(&db,db.entries[i].ssid,enabled);}
esp_err_t wifi_credentials_forget(void){wifi_known_network_db_t db;esp_err_t e=wifi_credentials_load_database(&db);if(e!=ESP_OK)return e;int i=newest(&db);if(i<0)return ESP_ERR_NVS_NOT_FOUND;return wifi_credentials_forget_network(&db,db.entries[i].ssid);}