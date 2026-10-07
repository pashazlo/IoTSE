#include "wifi_capture_export.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_netif_ip_addr.h"
#include "wifi_worker.h"
#include "file_service.h"

static httpd_handle_t s_server;
static char s_token[9];

static bool valid_name(const char *name)
{
    if (name == NULL) return false;
    size_t length = strlen(name);
    if (length <= 10U || length > 30U || memcmp(name, "wifi_", 5) != 0 ||
        memcmp(name + length - 5U, ".pcap", 5) != 0) return false;
    for (size_t i = 5U; i < length - 5U; ++i)
        if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

static bool token_valid(httpd_req_t *request)
{
    char query[32], token[12];
    size_t length = httpd_req_get_url_query_len(request);
    return length > 0 && length < sizeof(query) &&
           httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, "t", token, sizeof(token)) == ESP_OK &&
           strcmp(token, s_token) == 0;
}

static esp_err_t list_handler(httpd_req_t *request)
{
    if (!token_valid(request)) {
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        return httpd_resp_sendstr(request,
            "<!doctype html><meta name=viewport content='width=device-width'>"
            "<title>IoTSE export</title><h1>IoTSE PCAP export</h1>"
            "<form method=get><label>Token from device: "
            "<input name=t maxlength=8 minlength=8 required "
            "autocapitalize=off autocomplete=off></label> "
            "<button type=submit>Open</button></form>");
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(request,
        "<!doctype html><meta name=viewport content='width=device-width'>"
        "<title>IoTSE captures</title><h1>IoTSE PCAP captures</h1><ul>");
    char capture_dir[STORAGE_MANAGER_PATH_MAX];
    DIR *directory = file_service_resolve(STORAGE_ROLE_CAPTURE_WIFI_PCAP, NULL,
                                          capture_dir, sizeof(capture_dir)) == ESP_OK
        ? file_service_opendir_stream(capture_dir) : NULL;
    if (directory != NULL) {
        struct dirent *entry;
        char row[256];
        while ((entry = readdir(directory)) != NULL) {
            if (!valid_name(entry->d_name)) continue;
            snprintf(row, sizeof(row),
                     "<li><a href='/file/%.30s?t=%.8s'>%.30s</a></li>",
                     entry->d_name, s_token, entry->d_name);
            httpd_resp_sendstr_chunk(request, row);
        }
        file_service_closedir_stream(directory);
    }
    httpd_resp_sendstr_chunk(request, "</ul><p>Open downloaded files in Wireshark.</p>");
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t file_handler(httpd_req_t *request)
{
    if (!token_valid(request)) {
        httpd_resp_send_err(request, HTTPD_403_FORBIDDEN, "Invalid token");
        return ESP_OK;
    }
    const char *name = request->uri + strlen("/file/");
    char clean_name[31];
    size_t length = strcspn(name, "?");
    if (length == 0U || length >= sizeof(clean_name)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_OK;
    }
    memcpy(clean_name, name, length);
    clean_name[length] = '\0';
    if (!valid_name(clean_name)) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_OK;
    }
    char path[STORAGE_MANAGER_PATH_MAX], disposition[64];
    if (file_service_resolve(STORAGE_ROLE_CAPTURE_WIFI_PCAP, clean_name,
                             path, sizeof(path)) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }
    file_service_file_t input;
    if (file_service_open(path, "rb", &input) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }
    FILE *file = input.stream;
    httpd_resp_set_type(request, "application/vnd.tcpdump.pcap");
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"", clean_name);
    httpd_resp_set_hdr(request, "Content-Disposition", disposition);
    char buffer[1024];
    size_t read;
    esp_err_t result = ESP_OK;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (httpd_resp_send_chunk(request, buffer, read) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
    }
    (void)file_service_close(&input);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, NULL, 0);
    return result;
}

esp_err_t wifi_capture_export_start(char *url, uint32_t url_size)
{
    if (url == NULL || url_size == 0) return ESP_ERR_INVALID_ARG;
    if (s_server != NULL) return ESP_ERR_INVALID_STATE;
    wifi_connection_snapshot_t snapshot;
    if (!wifi_worker_get_connection_snapshot(&snapshot) ||
        !snapshot.info.connected) return ESP_ERR_INVALID_STATE;
    snprintf(s_token, sizeof(s_token), "%08lx", (unsigned long)esp_random());
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t list = {.uri = "/", .method = HTTP_GET,
                              .handler = list_handler};
    const httpd_uri_t file = {.uri = "/file/*", .method = HTTP_GET,
                              .handler = file_handler};
    if (httpd_register_uri_handler(s_server, &list) != ESP_OK ||
        httpd_register_uri_handler(s_server, &file) != ESP_OK) {
        wifi_capture_export_stop();
        return ESP_FAIL;
    }
    esp_ip4_addr_t ip = {.addr = snapshot.info.ip};
    snprintf(url, url_size, "http://" IPSTR ":8080/?t=%s", IP2STR(&ip), s_token);
    return ESP_OK;
}

void wifi_capture_export_stop(void)
{
    if (s_server != NULL) {
        (void)httpd_stop(s_server);
        s_server = NULL;
    }
    memset(s_token, 0, sizeof(s_token));
}

bool wifi_capture_export_is_running(void) { return s_server != NULL; }
