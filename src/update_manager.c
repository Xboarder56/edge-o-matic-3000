#include "update_manager.h"
#include "cJSON.h"
#include "config.h"
#include "eom-hal.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "version.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define MAX_HTTP_OUTPUT_BUFFER 4096

static const char* TAG = "update_manager";
static um_update_status_t _update_status = UM_UPDATE_NOT_CHECKED;

struct um_progress_cb_data {
    um_progress_callback_t* cb;
    void* arg;
    char** buffer;
    int* size;
};

/**
 * esp_http_client follows redirects but still delivers the redirect response's body through
 * HTTP_EVENT_ON_DATA / ON_FINISH. Those bodies must not be mistaken for the real response.
 */
static bool _is_redirect(esp_http_client_handle_t client) {
    int status = esp_http_client_get_status_code(client);
    return status >= 300 && status < 400;
}

esp_err_t _http_update_event_handler(esp_http_client_event_t* evt) {
    struct um_progress_cb_data* cb = (struct um_progress_cb_data*)evt->user_data;
    static int output_len = 0;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED: {
        output_len = 0;
        break;
    }

    case HTTP_EVENT_ON_DATA: {
        if (_is_redirect(evt->client)) break;

        if (!esp_http_client_is_chunked_response(evt->client)) {
            int content_length = esp_http_client_get_content_length(evt->client);
            if (*(cb->size) != content_length) {
                ESP_LOGI(
                    TAG, "Setting response length from: %d to: %d", *(cb->size), content_length
                );
                *(cb->size) = content_length;
            }

            output_len += evt->data_len;

            if (cb != NULL && cb->cb != NULL) {
                cb->cb(UM_PROG_PAYLOAD_DATA, output_len, *(cb->size), cb->arg);
            }
        } else {
            ESP_LOGI(TAG, "Chunked Data");
        }
        break;
    }
    default: break;
    }

    return ESP_OK;
}

esp_err_t _http_event_handler(esp_http_client_event_t* evt) {
    static int output_len; // Stores number of bytes read
    struct um_progress_cb_data* cb = (struct um_progress_cb_data*)evt->user_data;
    assert(cb != NULL);

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (_is_redirect(evt->client)) break;

        if (!esp_http_client_is_chunked_response(evt->client)) {
            int content_length = esp_http_client_get_content_length(evt->client);

            if (*(cb->buffer) == NULL) {
                if (*(cb->size) == 0) *(cb->size) = content_length;
                *(cb->buffer) = (char*)malloc(content_length + 1);
                output_len = 0;

                if (*(cb->buffer) == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                    return ESP_FAIL;
                } else {
                    ESP_LOGI(
                        TAG,
                        "Allocated request buffer: %d bytes (reported %d)",
                        content_length + 1,
                        *(cb->size)
                    );
                }
            }

            if (output_len + evt->data_len > *(cb->size)) {
                ESP_LOGE(TAG, "Possible buffer overflow!");
                return ESP_FAIL;
            }

            memcpy(*(cb->buffer) + output_len, evt->data, evt->data_len);
            output_len += evt->data_len;

            if (cb->cb != NULL) {
                cb->cb(UM_PROG_PREFLIGHT_DATA, output_len, content_length, cb->arg);
            }
        }

        break;

    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (_is_redirect(evt->client)) break;

        if (*(cb->buffer) != NULL) {
            // Response is accumulated in output_buffer. Uncomment the below line to print the
            // accumulated response
            // ESP_LOG_BUFFER_HEXDUMP(TAG, *(cb->buffer), output_len, ESP_LOG_INFO);
            (*(cb->buffer))[output_len] = '\0';
        }

        output_len = 0;

        if (cb->cb != NULL) {
            cb->cb(UM_PROG_PREFLIGHT_DONE, 0, 0, cb->arg);
        }

        break;

    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0) {
            if (*(cb->buffer) != NULL) {
                free(*(cb->buffer));
                *(cb->buffer) = NULL;
            }

            output_len = 0;
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);

            if (cb != NULL && cb->cb != NULL) {
                cb->cb(UM_PROG_PREFLIGHT_ERROR, err, mbedtls_err, cb->arg);
            }
        }
        break;

    default: ESP_LOGD(TAG, "HTTP Event: %d", evt->event_id); break;
    }

    return ESP_OK;
}

esp_err_t update_manager_update_from_storage(
    const char* path, um_progress_callback_t* progress_cb, void* cb_arg
) {
    if (!path) return ESP_ERR_INVALID_ARG;
    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) return ESP_ERR_NOT_FOUND;

    FILE* file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;

    esp_err_t err = ESP_FAIL;
    uint8_t* buffer = NULL;
    esp_ota_handle_t handle;
    bool started = false;
    const size_t chunk_size = 4096;
    if (fseek(file, 0, SEEK_END) != 0) goto cleanup;
    long size = ftell(file);
    if (size <= 0 || (unsigned long)size > partition->size) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_SET) != 0) goto cleanup;

    buffer = malloc(chunk_size);
    if (!buffer) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    err = esp_ota_begin(partition, size, &handle);
    if (err != ESP_OK) goto cleanup;
    started = true;
    if (progress_cb) progress_cb(UM_PROG_PREFLIGHT_DONE, 0, 0, cb_arg);

    long written = 0;
    size_t count;
    while ((count = fread(buffer, 1, chunk_size, file)) > 0) {
        if (count > (size_t)(size - written)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        err = esp_ota_write(handle, buffer, count);
        if (err != ESP_OK) break;
        written += count;
        if (progress_cb) progress_cb(UM_PROG_PAYLOAD_DATA, written, size, cb_arg);
    }
    if (err == ESP_OK && (ferror(file) || written != size)) err = ESP_FAIL;

cleanup:
    free(buffer);
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err != ESP_OK) {
        if (started) esp_ota_abort(handle);
        return err;
    }

    if (progress_cb) progress_cb(UM_PROG_PAYLOAD_DONE, 0, 0, cb_arg);
    // esp_ota_end consumes the handle, including when validation fails.
    err = esp_ota_end(handle);
    if (err != ESP_OK) return err;
    return esp_ota_set_boot_partition(partition);
}

esp_err_t update_manager_update_from_web(um_progress_callback_t* progress_cb, void* cb_arg) {
    char* buffer = NULL;
    int size = 0;
    esp_err_t err = ESP_OK;

    char* manifest_url = NULL;
    if (asiprintf(&manifest_url, "%s?current_version=%s", Config.remote_update_url, EOM_VERSION) < 0)
        return ESP_ERR_NO_MEM;

    struct um_progress_cb_data callback_data = {
        .cb = progress_cb,
        .arg = cb_arg,
        .buffer = &buffer,
        .size = &size,
    };

    esp_http_client_config_t data_config = {
        .url = manifest_url,
        .event_handler = _http_event_handler,
        .user_data = &callback_data,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&data_config);

    if (!client) {
        free(manifest_url);
        return ESP_ERR_NO_MEM;
    }
    err = esp_http_client_perform(client);
    esp_http_client_cleanup(client); // releases the connection (socket) and client memory
    free(manifest_url);

    if (err != ESP_OK || buffer == NULL) {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        free(buffer);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    ESP_LOGI(TAG, "Got %d bytes: %s", strlen(buffer), buffer);

    cJSON* data = cJSON_Parse(buffer);
    free(buffer);
    buffer = NULL;

    if (!cJSON_IsObject(data)) {
        ESP_LOGE(TAG, "Server response was not a valid JSON object.");
        cJSON_Delete(data);
        return ESP_FAIL;
    }

    if (cJSON_HasObjectItem(data, "version") && cJSON_HasObjectItem(data, "date")) {
        ESP_LOGI(
            TAG,
            "Downloading asset for version %s, released: %s",
            cJSON_GetObjectItem(data, "version")->valuestring,
            cJSON_GetObjectItem(data, "date")->valuestring
        );
    } else {
        ESP_LOGW(TAG, "Possibly improperly formatted data.");
    }

    if (!cJSON_HasObjectItem(data, "assets")) {
        ESP_LOGE(TAG, "Server response did not contain Asset key.");
        cJSON_Delete(data);
        return ESP_FAIL;
    }

    cJSON* assets = cJSON_GetObjectItem(data, "assets");
    cJSON* cursor = NULL;
    const char* url = NULL;

    cJSON_ArrayForEach(cursor, assets) {
        if (cJSON_HasObjectItem(cursor, "role") && cJSON_HasObjectItem(cursor, "url")) {
            char* role = cJSON_GetObjectItem(cursor, "role")->valuestring;
            ESP_LOGI(TAG, "Asset has role: %s", role);
            if (!strcmp(role, "image")) {
                url = cJSON_GetObjectItem(cursor, "url")->valuestring;
                break;
            }
        }
    }

    if (url == NULL) {
        ESP_LOGE(TAG, "Could not find an asset with an image role.");
        err = ESP_FAIL;
    } else {
        assert(*(callback_data.buffer) == NULL);

        esp_http_client_config_t config = {
            .url = url,
            .user_data = &callback_data,
            .event_handler = _http_update_event_handler,
            .keep_alive_enable = true,
            .skip_cert_common_name_check = true,
            .buffer_size_tx = 4096,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        ESP_LOGI(TAG, "Downloading update from: %s", config.url);
        err = esp_https_ota(&config);
    }

    cJSON_Delete(data);
    return err;
}

esp_err_t update_manager_check_latest_version(semver_t* version) {
    char* buffer = NULL;
    int size = 0;

    struct um_progress_cb_data callback_data = {
        .cb = NULL,
        .arg = NULL,
        .buffer = &buffer,
        .size = &size,
    };

    char* url = NULL;
    asiprintf(
        &url,
        "%s/%s?current_version=%s",
        Config.remote_update_url,
        "version.txt",
        EOM_VERSION
    );

    esp_http_client_config_t config = {
        .url = url,
        .user_data = &callback_data,
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    ESP_LOGI(TAG, "Checking for OTA updates: %s", config.url);

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client); // releases the connection (socket) and client memory
    free(url);

    if (err != ESP_OK || buffer == NULL) {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        free(buffer);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    ESP_LOGI(TAG, "Got %d bytes", size);
    // ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, size, ESP_LOG_INFO);

    char version_str[40] = "";

    if (semver_parse(buffer + (buffer[0] == 'v' ? 1 : 0), version) == -1) {
        ESP_LOGE(TAG, "Failed to parse version string \"%s\"!", buffer);
        free(buffer);
        return ESP_FAIL;
    }

    semver_render(version, version_str);
    free(buffer);

    ESP_LOGI(TAG, "Found version: %s", version_str);
    return ESP_OK;
}

um_update_status_t update_manager_get_status(void) {
    return _update_status;
}

um_update_status_t update_manager_check_online_updates() {
    semver_t v_remote = {};

    if (Config.remote_update_url == NULL || Config.remote_update_url[0] == '\0') {
        ESP_LOGI(TAG, "Online update checks disabled (remote_update_url is empty).");
        return UM_UPDATE_NOT_AVAILABLE;
    }

    if (update_manager_check_latest_version(&v_remote) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to check for remote updates.");
        goto umcheck_end;
    }

    if (!EOM_VERSION_VALID) {
        // A remote version exists but can't be compared against the local build.
        // Assume an update is available so a corrective firmware can always be pushed.
        ESP_LOGW(TAG, "Cannot compare versions — marking update available.");
        _update_status = UM_UPDATE_AVAILABLE;
        goto umcheck_end;
    }

    int compare = semver_compare(EOM_PARSED_VERSION, v_remote);

    char s_current[40] = { 0 };
    char s_remote[40] = { 0 };
    semver_render(&v_remote, s_remote);
    semver_render(&EOM_PARSED_VERSION, s_current);

    ESP_LOGI(TAG, "Comparing version %s <=> %s == %d", s_current, s_remote, compare);

    if (compare < 0) {
        _update_status = UM_UPDATE_AVAILABLE;
        ESP_LOGI(TAG, "An update for your software is available!");
    } else if (compare == 0) {
        _update_status = UM_UPDATE_NOT_AVAILABLE;
    }

umcheck_end:
    semver_free(&v_remote);
    return _update_status;
}

um_update_status_t update_manager_check_for_updates(void) {
    um_update_status_t status = UM_UPDATE_NOT_CHECKED;

    status = update_manager_check_online_updates();
    if (status != UM_UPDATE_NOT_AVAILABLE) return status;

    status = update_manager_check_local_updates();
    return status;
}

um_update_status_t
update_manager_list_local_updates(um_update_callback_t* on_result, void* cb_arg) {
    DIR* d;
    struct dirent* dir;
    um_update_status_t status = UM_UPDATE_NOT_AVAILABLE;

    d = opendir(eom_hal_get_sd_mount_point());

    if (d) {
        while ((dir = readdir(d)) != NULL) {
            size_t name_len = strlen(dir->d_name);
            if (dir->d_type != DT_REG || name_len < 4 ||
                strcmp(dir->d_name + name_len - 4, ".bin") || dir->d_name[0] == '.')
                continue;

            char* path;
            asiprintf(&path, "%s/%s", eom_hal_get_sd_mount_point(), dir->d_name);

            if (path == NULL) {
                ESP_LOGE(TAG, "Allocate failed.");
                closedir(d);
                return UM_UPDATE_ERROR;
            }

            ESP_LOGI(TAG, "Checking for local updates: %s", path);

            FILE* update_bin = fopen(path, "rb");

            if (update_bin == NULL) {
                free(path);
                continue;
            }

            ESP_LOGI(TAG, "Checking binary header...");
            const size_t offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            esp_app_desc_t app_desc = { 0 };

            fseek(update_bin, offset, SEEK_SET);
            fread(&app_desc, sizeof(esp_app_desc_t), 1, update_bin);
            fclose(update_bin);

            if (app_desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
                ESP_LOG_BUFFER_HEXDUMP(TAG, &app_desc, sizeof(esp_app_desc_t), ESP_LOG_INFO);
                ESP_LOGI(TAG, "Project Name: %s", app_desc.project_name);
                ESP_LOGI(TAG, "Project Version: %s", app_desc.version);
                ESP_LOGI(TAG, "Compile Date: %s", app_desc.date);
                ESP_LOGI(TAG, "Compile Time: %s", app_desc.time);

                um_update_info_t update_info = {
                    .date = app_desc.date,
                    .path = path,
                    .time = app_desc.time,
                    .version = app_desc.version,
                };

                if (on_result != NULL) {
                    on_result(&update_info, cb_arg);
                }

                status = UM_UPDATE_AVAILABLE;
            } else {
                ESP_LOGI(TAG, "File was not a valid application.");
            }

            free(path);
        }

        closedir(d);
    }

    return status;
}

um_update_status_t update_manager_check_local_updates(void) {
    return update_manager_list_local_updates(NULL, NULL);
}