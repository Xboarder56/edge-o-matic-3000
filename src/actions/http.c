/**
 * HTTP(S) client for plugins.
 *
 * One request at a time, run on a short-lived worker task so the plugin (and the main loop) never
 * blocks. The response is delivered later as the "http_response" event to the plugin, or driver
 * instance, that made the request, on the same task that runs that plugin's other events. The
 * response body and its JSON are only available inside that event handler; they are freed as soon
 * as the handler returns.
 *
 * Memory is the constraint: an HTTPS request to an internet host needs more than 42 KB of heap
 * while it runs (TLS buffers plus the worker's stack); plain HTTP needs ~15 KB. Requests are refused with
 * HTTP_REQ_NO_MEMORY instead of being attempted when the heap is too low. Nothing is allocated
 * until a plugin makes a request.
 */
#include "actions/http.h"
#include "cJSON.h"
#include "drivers/plugin_driver.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "mt_actions_internal.h"
#include "system/action_manager.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char* TAG = "actions:http";

#define PERM_NET_HTTP "net:http"

#define HTTP_URL_MAX 256
#define HTTP_BODY_MAX 1024
#define HTTP_RESPONSE_MAX 4096
#define HTTP_HEADERS_MAX 4
#define HTTP_HEADER_NAME_MAX 32
#define HTTP_HEADER_VALUE_MAX 128
#define HTTP_TIMEOUT_MS 10000

// Free heap needed before starting a request. Measured on an EOM3K:
//   plain HTTP                         ~15 KB (worker stack, client buffers, socket)
//   HTTPS to GitHub Pages (verified)   > 42 KB with mbedTLS's fixed buffers (16 KB receive
//                                      buffer in one block, 4 KB send buffer, handshake)
// TLS keeps fixed buffers: CONFIG_MBEDTLS_DYNAMIC_BUFFER lowered the handshake peak to ~39 KB but
// made the 1.4 MB online firmware update fail (the 16 KB buffer is re-allocated per record and
// the heap fragments). The thresholds leave ~8 KB spare at the worst measured peak, so HTTPS is
// refused (HTTP_REQ_NO_MEMORY) when more than a small plugin or two is loaded.
#define HTTP_MIN_FREE_HEAP 12000
#define HTTPS_MIN_FREE_HEAP 52000
#define HTTPS_MIN_LARGEST_BLOCK 20000

// Measured stack use: plain HTTP ~2.2 KB, HTTPS ~4.5 KB.
#define HTTP_TASK_STACK 4096
#define HTTPS_TASK_STACK 6144

// Lowest priority: the TLS handshake is CPU-heavy and must never delay the main loop (priority 1:
// sensing, motor control) or the plugin task (BLE drivers, e.g. a Coyote's 100 ms stream).
// Network waits block, so the request still completes promptly.
#define HTTP_TASK_PRIORITY tskIDLE_PRIORITY

#define HTTP_FLAG_INSECURE (1 << 0)

typedef struct {
    char name[HTTP_HEADER_NAME_MAX];
    char value[HTTP_HEADER_VALUE_MAX];
} http_header_t;

typedef enum {
    REQ_IDLE = 0,
    REQ_RUNNING, // worker task owns the request
    REQ_DONE,    // waiting to be delivered to the requester
} req_state_t;

typedef struct {
    req_state_t state;
    int id;

    // Requester. instance != NULL for a BLE driver instance, else a feature plugin.
    mta_plugin_t* plugin;
    mta_driver_instance_t* instance;

    // Request
    esp_http_client_method_t method;
    bool https;
    char* url;
    char* body;
    int flags;
    http_header_t* headers; // owned; NULL when the request has none
    uint8_t header_count;

    // Response
    int status; // HTTP status, or a negative http_err_t
    char* response;
    int response_len;
    bool truncated;
    size_t min_free_heap; // lowest free heap seen while the request ran (diagnostics)
} http_req_t;

static http_req_t _req = { 0 };
static int _next_id = 1;
static SemaphoreHandle_t _lock = NULL;
static volatile int _tls_holds = 0; // > 0: new HTTPS requests are refused (BLE connect running)

// Headers staged by http_set_header() for the next request of the same plugin.
// Allocated by the first http_set_header and handed to the request that uses it.
static mta_plugin_t* _staged_plugin = NULL;
static http_header_t* _staged = NULL;
static uint8_t _staged_count = 0;

// Response being delivered: getters only answer the plugin whose handler is running.
static http_req_t* _delivering = NULL;
static mta_plugin_t* _delivering_plugin = NULL;
static cJSON* _delivering_json = NULL;
static bool _delivering_json_parsed = false;

static void lock(void) {
    xSemaphoreTake(_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(_lock);
}

static void req_free_buffers(http_req_t* req) {
    free(req->url);
    free(req->body);
    free(req->response);
    free(req->headers);
    req->url = NULL;
    req->body = NULL;
    req->response = NULL;
    req->headers = NULL;
    req->header_count = 0;
    req->response_len = 0;
}

// ---------------------------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------------------------

static void sample_heap(http_req_t* req) {
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (req->min_free_heap == 0 || free_heap < req->min_free_heap) req->min_free_heap = free_heap;
}

static void append_response(http_req_t* req, const char* data, int len) {
    int room = HTTP_RESPONSE_MAX - req->response_len;
    int take = len < room ? len : room;
    if (take < len) req->truncated = true;
    if (take <= 0) return;

    char* grown = realloc(req->response, req->response_len + take + 1);
    if (grown == NULL) {
        req->truncated = true;
        return;
    }

    req->response = grown;
    memcpy(req->response + req->response_len, data, take);
    req->response_len += take;
    req->response[req->response_len] = '\0';
}

// Used by the HTTPS path (esp_http_client_perform).
// Samples free heap every tick while a request runs, so the log reports each request's real
// low point (the TLS handshake peak happens inside esp-tls, between client events).
static void heap_sampler_cb(TimerHandle_t timer) {
    sample_heap((http_req_t*)pvTimerGetTimerID(timer));
}

static esp_err_t _on_http_event(esp_http_client_event_t* evt) {
    http_req_t* req = (http_req_t*)evt->user_data;
    sample_heap(req);

    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0 || !req->https) return ESP_OK;

    // Bodies of redirect responses are not the response.
    int status = esp_http_client_get_status_code(evt->client);
    if (status >= 300 && status < 400) return ESP_OK;

    append_response(req, evt->data, evt->data_len);
    return ESP_OK;
}

/**
 * Plain HTTP, step by step. Unlike esp_http_client_perform this never follows a redirect (one to
 * https:// would need far more memory than was checked for), and it reads the body for every
 * status, so error bodies (e.g. a 409 with a JSON error code) reach the plugin.
 */
static esp_err_t http_plain(esp_http_client_handle_t client, http_req_t* req, int* status) {
    int body_len = req->body ? strlen(req->body) : 0;
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) return err;

    if (body_len > 0 && esp_http_client_write(client, req->body, body_len) != body_len) {
        esp_http_client_close(client);
        return ESP_FAIL;
    }

    if (esp_http_client_fetch_headers(client) < 0) {
        esp_http_client_close(client);
        return ESP_FAIL;
    }

    *status = esp_http_client_get_status_code(client);
    sample_heap(req);

    char chunk[256];
    int n;
    while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
        append_response(req, chunk, n);
    }

    esp_http_client_close(client);
    return n < 0 ? ESP_FAIL : ESP_OK;
}

static int map_error(esp_err_t err) {
    switch (err) {
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_TIMEOUT: return HTTP_ERR_TIMEOUT;
    case ESP_ERR_NO_MEM: return HTTP_ERR_NO_MEMORY;
    default: return HTTP_ERR_CONNECTION;
    }
}

static void http_worker(void* arg) {
    http_req_t* req = (http_req_t*)arg;
    bool https = !strncasecmp(req->url, "https://", 8);

    esp_http_client_config_t config = {
        .url = req->url,
        .method = req->method,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = _on_http_event,
        .user_data = req,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .max_redirection_count = 3,
        .keep_alive_enable = false,
    };

    if (https) {
        if (req->flags & HTTP_FLAG_INSECURE) {
            // No CA bundle: esp-tls skips server verification (CONFIG_ESP_TLS_INSECURE).
            config.skip_cert_common_name_check = true;
        } else {
            config.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }

    int status = HTTP_ERR_CONNECTION;
    TimerHandle_t sampler = xTimerCreate("http_heap", 1, pdTRUE, req, heap_sampler_cb);
    if (sampler != NULL) xTimerStart(sampler, 0);

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL) {
        status = HTTP_ERR_NO_MEMORY;
    } else {
        bool has_content_type = false;

        for (uint8_t i = 0; i < req->header_count; i++) {
            esp_http_client_set_header(client, req->headers[i].name, req->headers[i].value);
            if (!strcasecmp(req->headers[i].name, "Content-Type")) has_content_type = true;
        }

        if (req->body != NULL && req->body[0] != '\0') {
            if (!has_content_type) {
                // JSON if it looks like JSON, otherwise a form body.
                const char* p = req->body;
                while (isspace((unsigned char)*p)) p++;
                esp_http_client_set_header(
                    client,
                    "Content-Type",
                    (*p == '{' || *p == '[') ? "application/json"
                                             : "application/x-www-form-urlencoded"
                );
            }
            if (https) esp_http_client_set_post_field(client, req->body, strlen(req->body));
        }

        sample_heap(req);
        esp_err_t err;

        if (https) {
            err = esp_http_client_perform(client); // follows up to 3 redirects
            if (err == ESP_OK) status = esp_http_client_get_status_code(client);
        } else {
            err = http_plain(client, req, &status);
        }
        sample_heap(req);

        if (err != ESP_OK) {
            status = map_error(err);
            ESP_LOGW(TAG, "Request %d failed: %s", req->id, esp_err_to_name(err));
        }

        esp_http_client_cleanup(client);
    }

    if (sampler != NULL) xTimerDelete(sampler, portMAX_DELAY);

    ESP_LOGI(
        TAG,
        "Request %d done: status=%d, %d bytes%s, lowest free heap during request %u, stack unused %u",
        req->id,
        status,
        req->response_len,
        req->truncated ? " (truncated)" : "",
        req->min_free_heap,
        uxTaskGetStackHighWaterMark(NULL)
    );

    lock();
    req->status = status;
    req->state = REQ_DONE;
    unlock();

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------------------------
// Delivery
// ---------------------------------------------------------------------------------------------

typedef struct {
    mta_driver_instance_t* target;
    bool found;
} instance_find_t;

static void find_instance_cb(mta_driver_instance_t* instance, void* arg) {
    instance_find_t* f = (instance_find_t*)arg;
    if (instance == f->target) f->found = true;
}

static bool instance_exists(mta_driver_instance_t* instance) {
    instance_find_t f = { .target = instance, .found = false };
    mta_driver_instance_foreach(find_instance_cb, &f);
    return f.found;
}

void action_http_poll(bool driver_task) {
    if (_lock == NULL) return;

    lock();
    bool ready = _req.state == REQ_DONE && ((_req.instance != NULL) == driver_task);
    unlock();

    if (!ready) return;

    // The worker is finished with _req; only this task touches it until it is released.
    _delivering = &_req;
    _delivering_plugin = _req.plugin;
    _delivering_json = NULL;
    _delivering_json_parsed = false;

    if (driver_task) {
        if (instance_exists(_req.instance)) {
            mta_driver_instance_dispatch(_req.instance, "http_response", _req.status, NULL);
        } else {
            ESP_LOGW(TAG, "Request %d: driver instance is gone, dropping response", _req.id);
        }
    } else if (action_manager_invoke_plugin_event(_req.plugin, "http_response", _req.status) < -1) {
        ESP_LOGW(TAG, "Request %d: plugin is gone, dropping response", _req.id);
    }

    cJSON_Delete(_delivering_json);
    _delivering_json = NULL;
    _delivering = NULL;
    _delivering_plugin = NULL;

    lock();
    req_free_buffers(&_req);
    _req.state = REQ_IDLE;
    unlock();
}

static http_req_t* delivering_for(mta_plugin_t* plugin) {
    return (_delivering != NULL && _delivering_plugin == plugin) ? _delivering : NULL;
}

static cJSON* delivering_json(mta_plugin_t* plugin) {
    http_req_t* req = delivering_for(plugin);
    if (req == NULL) return NULL;

    if (!_delivering_json_parsed) {
        _delivering_json_parsed = true;
        if (req->response != NULL) _delivering_json = cJSON_Parse(req->response);
    }

    return _delivering_json;
}

/**
 * Resolve a dotted path ("timer.state", "items.0.name") against a JSON value.
 */
static cJSON* json_path(cJSON* root, const char* path) {
    if (root == NULL || path == NULL) return NULL;
    if (path[0] == '\0') return root;

    char key[48];
    cJSON* node = root;
    const char* p = path;

    while (node != NULL && *p) {
        size_t n = strcspn(p, ".");
        if (n == 0 || n >= sizeof(key)) return NULL;
        memcpy(key, p, n);
        key[n] = '\0';
        p += n;
        if (*p == '.') p++;

        if (cJSON_IsArray(node)) {
            char* end = NULL;
            long index = strtol(key, &end, 10);
            if (*end != '\0' || index < 0) return NULL;
            node = cJSON_GetArrayItem(node, (int)index);
        } else if (cJSON_IsObject(node)) {
            node = cJSON_GetObjectItemCaseSensitive(node, key);
        } else {
            return NULL;
        }
    }

    return node;
}

// ---------------------------------------------------------------------------------------------
// Host functions
// ---------------------------------------------------------------------------------------------

static bool parse_method(const char* s, esp_http_client_method_t* out) {
    static const struct {
        const char* name;
        esp_http_client_method_t method;
    } methods[] = {
        { "GET", HTTP_METHOD_GET },     { "POST", HTTP_METHOD_POST },
        { "PUT", HTTP_METHOD_PUT },     { "PATCH", HTTP_METHOD_PATCH },
        { "DELETE", HTTP_METHOD_DELETE }, { "HEAD", HTTP_METHOD_HEAD },
    };

    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (!strcasecmp(s, methods[i].name)) {
            *out = methods[i].method;
            return true;
        }
    }

    return false;
}

static mta_driver_instance_t* scope_driver_instance(mta_scope_t* scope) {
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)mta_scope_get_user_data(scope);
    if (ctx == NULL || ctx->peer == NULL) return NULL;
    return (mta_driver_instance_t*)ctx->peer->driver_state;
}

/**
 * Start an HTTP or HTTPS request. Returns at once; the result arrives as the http_response event.
 * Only one request can run at a time on the device. A body that looks like JSON is sent as
 * application/json, otherwise as a form, unless a Content-Type header was set with
 * http_set_header. HTTPS requests follow up to 3 redirects; plain HTTP requests do not (the
 * 3xx status is returned). Flags: 1 = skip TLS certificate verification (HTTPS only).
 *
 * @plugin http_request
 * @module http
 * @arg method:string GET, POST, PUT, PATCH, DELETE or HEAD
 * @arg url:string http:// or https:// URL (up to 255 characters)
 * @arg body:string? Request body (up to 1024 bytes), "" for none
 * @arg flags:int? 1 = skip TLS certificate verification
 * @returns int Request ID (> 0), -1 if another request is running, -2 for an invalid method or
 * URL, -3 if there is not enough free memory
 */
static int
host_http_request(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "method", MTA_KIND_STRING },
        { "url", MTA_KIND_STRING },
        { "body", MTA_KIND_STRING },
        { "flags", MTA_KIND_INT },
    };

    if (arg_count < 2 || arg_count > 4) {
        return mta_raise(
            plugin, MTA_RT_ERR_ARG_COUNT_MISMATCH, "http_request: expected 2-4 args, got %d", arg_count
        );
    }

    mta_argv_t v[4];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, arg_count, v, "http_request");
    if (ret != 0) return ret;

    const char* url = v[1].val.s;
    const char* body = arg_count >= 3 ? v[2].val.s : NULL;
    int flags = arg_count >= 4 ? v[3].val.i : 0;
    esp_http_client_method_t method;

    bool https = url && !strncasecmp(url, "https://", 8);
    bool http = url && !strncasecmp(url, "http://", 7);

    if (!v[0].val.s || !parse_method(v[0].val.s, &method) || !(http || https) ||
        strlen(url) >= HTTP_URL_MAX || (body && strlen(body) > HTTP_BODY_MAX)) {
        return mta_return_int(plugin, scope, HTTP_REQ_INVALID);
    }

    // Busy first: a running request is itself using the memory checked below.
    lock();
    bool busy = _req.state != REQ_IDLE || (https && _tls_holds > 0);
    unlock();
    if (busy) return mta_return_int(plugin, scope, HTTP_REQ_BUSY);

    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    if (free_heap < (https ? HTTPS_MIN_FREE_HEAP : HTTP_MIN_FREE_HEAP) ||
        (https && largest < HTTPS_MIN_LARGEST_BLOCK)) {
        ESP_LOGW(
            TAG, "Not enough memory for %s request (free %u, largest %u)", https ? "HTTPS" : "HTTP",
            free_heap, largest
        );
        return mta_return_int(plugin, scope, HTTP_REQ_NO_MEMORY);
    }

    lock();

    if (_req.state != REQ_IDLE || (https && _tls_holds > 0)) {
        unlock();
        return mta_return_int(plugin, scope, HTTP_REQ_BUSY);
    }

    memset(&_req, 0, sizeof(_req));
    _req.url = strdup(url);
    _req.body = (body && body[0]) ? strdup(body) : NULL;

    if (_req.url == NULL || (body && body[0] && _req.body == NULL)) {
        req_free_buffers(&_req);
        unlock();
        return mta_return_int(plugin, scope, HTTP_REQ_NO_MEMORY);
    }

    _req.id = _next_id++;
    if (_next_id <= 0) _next_id = 1;
    _req.plugin = plugin;
    _req.instance = scope_driver_instance(scope);
    _req.method = method;
    _req.https = https;
    _req.flags = flags;

    if (_staged_plugin == plugin && _staged != NULL) {
        _req.headers = _staged; // the request takes ownership
        _req.header_count = _staged_count;
    } else {
        free(_staged);
    }
    _staged = NULL;
    _staged_plugin = NULL;
    _staged_count = 0;

    _req.state = REQ_RUNNING;
    int id = _req.id;
    unlock();

    ESP_LOGI(
        TAG, "Request %d: %s %s%s", id, v[0].val.s, url,
        (https && (flags & HTTP_FLAG_INSECURE)) ? " (TLS verification off)" : ""
    );

    if (xTaskCreate(
            http_worker, "plugin_http", https ? HTTPS_TASK_STACK : HTTP_TASK_STACK, &_req,
            HTTP_TASK_PRIORITY, NULL
        ) != pdPASS) {
        lock();
        req_free_buffers(&_req);
        _req.state = REQ_IDLE;
        unlock();
        return mta_return_int(plugin, scope, HTTP_REQ_NO_MEMORY);
    }

    return mta_return_int(plugin, scope, id);
}

/**
 * Add a header to this plugin's next http_request (up to 4 headers; name up to 31 bytes, value
 * up to 127 bytes). Setting the same name again replaces it.
 *
 * @plugin http_set_header
 * @module http
 * @arg name:string Header name
 * @arg value:string Header value
 * @returns int 0 on success, -1 if there is no room or the header is too long
 */
static int
host_http_set_header(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "name", MTA_KIND_STRING },
        { "value", MTA_KIND_STRING },
    };

    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 2, v, "http_set_header");
    if (ret != 0) return ret;

    const char* name = v[0].val.s;
    const char* value = v[1].val.s;

    if (!name || !value || !name[0] || strlen(name) >= HTTP_HEADER_NAME_MAX ||
        strlen(value) >= HTTP_HEADER_VALUE_MAX || strpbrk(name, "\r\n:") || strpbrk(value, "\r\n")) {
        return mta_return_int(plugin, scope, -1);
    }

    lock();

    if (_staged_plugin != plugin) {
        _staged_plugin = plugin;
        _staged_count = 0;
    }

    if (_staged == NULL) {
        _staged = calloc(HTTP_HEADERS_MAX, sizeof(http_header_t));
        if (_staged == NULL) {
            _staged_plugin = NULL;
            unlock();
            return mta_return_int(plugin, scope, -1);
        }
    }

    int slot = -1;
    for (uint8_t i = 0; i < _staged_count; i++) {
        if (!strcasecmp(_staged[i].name, name)) slot = i;
    }

    if (slot < 0 && _staged_count < HTTP_HEADERS_MAX) slot = _staged_count++;

    if (slot >= 0) {
        strlcpy(_staged[slot].name, name, HTTP_HEADER_NAME_MAX);
        strlcpy(_staged[slot].value, value, HTTP_HEADER_VALUE_MAX);
    }

    unlock();
    return mta_return_int(plugin, scope, slot >= 0 ? 0 : -1);
}

/**
 * Whether an HTTP request (from any plugin) is running or waiting to be delivered.
 *
 * @plugin http_busy
 * @module http
 * @returns int 1 if busy, 0 if a new request can start
 */
static int
host_http_busy(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "http_busy");
    if (ret != 0) return ret;

    lock();
    int busy = _req.state != REQ_IDLE;
    unlock();
    return mta_return_int(plugin, scope, busy);
}

/**
 * ID of the request whose response is being delivered (inside http_response), else 0.
 *
 * @plugin http_response_id
 * @module http
 * @returns int Request ID returned by http_request
 */
static int
host_http_response_id(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "http_response_id");
    if (ret != 0) return ret;

    http_req_t* req = delivering_for(plugin);
    return mta_return_int(plugin, scope, req ? req->id : 0);
}

/**
 * Response body as text (inside http_response). Bodies are capped at 4096 bytes.
 *
 * @plugin http_response_body
 * @module http
 * @returns string Response body, "" if none
 */
static int host_http_response_body(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "http_response_body");
    if (ret != 0) return ret;

    http_req_t* req = delivering_for(plugin);
    return mta_return_string(plugin, scope, (req && req->response) ? req->response : "");
}

/**
 * Response body length in bytes (inside http_response).
 *
 * @plugin http_response_length
 * @module http
 * @returns int Bytes received (at most 4096), -1 if the body was cut off at 4096
 */
static int host_http_response_length(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "http_response_length");
    if (ret != 0) return ret;

    http_req_t* req = delivering_for(plugin);
    if (req == NULL) return mta_return_int(plugin, scope, 0);
    return mta_return_int(plugin, scope, req->truncated ? -1 : req->response_len);
}

/**
 * Copy the response body into a byte array (inside http_response).
 *
 * @plugin http_response_copy
 * @module http
 * @arg out:array Destination array; copies up to its capacity
 * @returns int Bytes copied
 */
static int host_http_response_copy(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = { { "out", MTA_KIND_ARRAY } };
    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "http_response_copy");
    if (ret != 0) return ret;

    http_req_t* req = delivering_for(plugin);
    mta_array_t* out = v[0].val.arr;
    int n = 0;

    if (req && req->response) {
        n = req->response_len < out->capacity ? req->response_len : out->capacity;
        for (int i = 0; i < n; i++) out->data[i] = (uint8_t)req->response[i];
    }

    out->length = n;
    return mta_return_int(plugin, scope, n);
}

/**
 * Whether the JSON response has a value at a dotted path (inside http_response).
 *
 * @plugin http_json_has
 * @module http
 * @arg path:string Dotted path, e.g. "state" or "items.0.name"
 * @returns int 1 if present (including null), 0 if not or the body is not JSON
 */
static int
host_http_json_has(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "path", MTA_KIND_STRING } };
    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "http_json_has");
    if (ret != 0) return ret;

    return mta_return_int(plugin, scope, json_path(delivering_json(plugin), v[0].val.s) != NULL);
}

/**
 * Read a JSON value as text (inside http_response). Strings are returned as-is, numbers and
 * booleans as text ("12", "true"), objects and arrays as compact JSON.
 *
 * @plugin http_json_string
 * @module http
 * @arg path:string Dotted path, e.g. "state" or "items.0.name"
 * @returns string Value, "" if missing
 */
static int
host_http_json_string(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "path", MTA_KIND_STRING } };
    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "http_json_string");
    if (ret != 0) return ret;

    cJSON* node = json_path(delivering_json(plugin), v[0].val.s);

    if (node == NULL || cJSON_IsNull(node)) return mta_return_string(plugin, scope, "");
    if (cJSON_IsString(node)) return mta_return_string(plugin, scope, node->valuestring);
    if (cJSON_IsBool(node)) return mta_return_string(plugin, scope, cJSON_IsTrue(node) ? "true" : "false");

    char* text = cJSON_PrintUnformatted(node);
    ret = mta_return_string(plugin, scope, text ? text : "");
    free(text);
    return ret;
}

/**
 * Read a JSON value as an integer (inside http_response). Numbers are truncated, booleans are
 * 1/0, numeric strings are parsed.
 *
 * @plugin http_json_int
 * @module http
 * @arg path:string Dotted path, e.g. "remaining_ms"
 * @returns int Value, 0 if missing or not a number
 */
static int
host_http_json_int(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "path", MTA_KIND_STRING } };
    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "http_json_int");
    if (ret != 0) return ret;

    cJSON* node = json_path(delivering_json(plugin), v[0].val.s);
    int32_t value = 0;

    if (cJSON_IsNumber(node)) {
        double d = node->valuedouble;
        value = d > 2147483647.0 ? 2147483647 : d < -2147483648.0 ? INT32_MIN : (int32_t)d;
    } else if (cJSON_IsBool(node)) {
        value = cJSON_IsTrue(node);
    } else if (cJSON_IsString(node) && node->valuestring) {
        value = (int32_t)strtol(node->valuestring, NULL, 10);
    }

    return mta_return_int(plugin, scope, value);
}

void action_http_hold_tls(bool hold) {
    if (_lock == NULL) return;
    lock();
    _tls_holds += hold ? 1 : -1;
    if (_tls_holds < 0) _tls_holds = 0;
    unlock();
}

bool action_http_wait_tls_idle(uint32_t timeout_ms) {
    if (_lock == NULL) return true;

    TickType_t start = xTaskGetTickCount();

    while (true) {
        lock();
        bool running = _req.state == REQ_RUNNING && _req.https;
        unlock();

        if (!running) return true;
        if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void action_http_init(void) {
    _lock = xSemaphoreCreateMutex();

    mta_register_system_function("http_request", host_http_request, PERM_NET_HTTP);
    mta_register_system_function("http_set_header", host_http_set_header, PERM_NET_HTTP);
    mta_register_system_function("http_busy", host_http_busy, PERM_NET_HTTP);
    mta_register_system_function("http_response_id", host_http_response_id, PERM_NET_HTTP);
    mta_register_system_function("http_response_body", host_http_response_body, PERM_NET_HTTP);
    mta_register_system_function("http_response_length", host_http_response_length, PERM_NET_HTTP);
    mta_register_system_function("http_response_copy", host_http_response_copy, PERM_NET_HTTP);
    mta_register_system_function("http_json_has", host_http_json_has, PERM_NET_HTTP);
    mta_register_system_function("http_json_string", host_http_json_string, PERM_NET_HTTP);
    mta_register_system_function("http_json_int", host_http_json_int, PERM_NET_HTTP);

    /**
     * Fires when this plugin's HTTP request finishes. Read the body with http_response_body,
     * http_json_string or http_json_int inside the handler; it is freed afterward.
     * @event http_response
     * @module http
     * @payload status:int HTTP status code, or -1 connection failed, -2 timed out, -3 out of memory
     */
    mta_register_event("http_response", PERM_NET_HTTP);
}
