#include "system/websocket_handler.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "update_mode.h"
#include "util/list.h"
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static const char* TAG = "websocket_handler";

static list_t _cmd_list = LIST_DEFAULT();

// Connected clients. Only ever touched on the httpd task (open/close callbacks, request
// handlers and queued broadcast work), so it needs no locking of its own.
static list_t _client_list = LIST_DEFAULT();

// Running server, used to queue broadcasts onto the httpd task. Guarded by _server_lock so a
// broadcast can never be queued on a server that is being stopped.
static httpd_handle_t _server = NULL;
static SemaphoreHandle_t _server_lock = NULL;

// Broadcast jobs waiting to run on the httpd task. Readings are coalesced (at most one pending)
// and the total is capped, so a busy or stalled httpd task cannot accumulate a backlog.
#define WS_BROADCAST_MAX_PENDING 8
static uint8_t _pending_jobs = 0;
static bool _readings_pending = false;

// TCP keepalive: detect peers that vanished without closing (sleeping phone, WiFi drop) in
// about KEEPIDLE + KEEPINTVL * KEEPCNT seconds, so their slot is freed.
#define WS_KEEPALIVE_IDLE_S 10
#define WS_KEEPALIVE_INTERVAL_S 5
#define WS_KEEPALIVE_COUNT 3

// How long a client's send buffer may stay continuously full (EAGAIN) before we treat the
// peer as stalled and close its session. Must outlast normal 2.4 GHz Wi-Fi retransmissions
// (~1.5 s RTO) and BLE scan coexistence pauses without blocking the httpd task.
#define WS_CLIENT_STALL_TIMEOUT_MS 5000

// Maximum time to wait to finish the tail of a partially written WebSocket frame (0 < sent <
// total) so framing is preserved across brief buffer backpressure while keeping httpd bounded.
#define WS_PARTIAL_SEND_TIMEOUT_MS 350

typedef struct {
    char* payload;
    size_t len;
    int broadcast_flags;
} ws_broadcast_job_t;

static void _lock(void) {
    if (_server_lock == NULL) _server_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(_server_lock, portMAX_DELAY);
}

static void _unlock(void) {
    xSemaphoreGive(_server_lock);
}

void websocket_server_started(httpd_handle_t server) {
    _lock();
    _server = server;
    _pending_jobs = 0;
    _readings_pending = false;
    _unlock();
}

void websocket_server_stopping(void) {
    _lock();
    _server = NULL;
    _unlock();
}

bool _is_websocket(websocket_client_t* client) {
    return httpd_ws_get_fd_info(client->server, client->fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

esp_err_t websocket_send_to_client(websocket_client_t* client, const char* msg) {
    if (!_is_websocket(client)) {
        return ESP_ERR_HTTPD_INVALID_REQ;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)msg;
    ws_pkt.len = strlen(msg);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.final = true;

    return httpd_ws_send_frame_async(client->server, client->fd, &ws_pkt);
}

/**
 * Send one complete text frame without blocking the httpd task on a dead reader.
 *
 * - If the whole frame fits at once, clear any stall timer and return.
 * - If the send buffer is temporarily full (0 bytes written / EAGAIN), drop this broadcast
 *   frame cleanly: framing is still intact, and Wi-Fi/BLE coexistence or a single TCP
 *   retransmit (~1.5 s) will drain shortly. Only disconnect if the buffer stays full
 *   continuously for WS_CLIENT_STALL_TIMEOUT_MS.
 * - If lwIP accepted part of the frame (0 < sent < total), finish writing the remaining
 *   bytes within WS_PARTIAL_SEND_TIMEOUT_MS so the WebSocket stream is never left half-framed;
 *   disconnect only if the peer still cannot take those remaining bytes.
 */
static esp_err_t _send_frame_nonblocking(websocket_client_t* client, const char* payload, size_t len) {
    uint8_t header[4];
    size_t header_len;

    header[0] = 0x80 | HTTPD_WS_TYPE_TEXT; // FIN + text
    if (len <= 125) {
        header[1] = len;
        header_len = 2;
    } else if (len <= 0xFFFF) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len = 4;
    } else {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total = header_len + len;
    char* frame = malloc(total);
    if (frame == NULL) return ESP_ERR_NO_MEM;

    memcpy(frame, header, header_len);
    memcpy(frame + header_len, payload, len);

    int sent = httpd_socket_send(client->server, client->fd, frame, total, MSG_DONTWAIT);

    if (sent == (int)total) {
        free(frame);
        client->stall_since_ms = 0;
        return ESP_OK;
    }

    if (sent == HTTPD_SOCK_ERR_TIMEOUT || sent == 0) {
        free(frame);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (client->stall_since_ms == 0) {
            client->stall_since_ms = now_ms;
        }
        if (now_ms - client->stall_since_ms < WS_CLIENT_STALL_TIMEOUT_MS) {
            return ESP_OK;
        }
        ESP_LOGW(
            TAG,
            "Client fd %d stalled for %lld ms, disconnecting",
            client->fd,
            (long long)(now_ms - client->stall_since_ms)
        );
        httpd_sess_trigger_close(client->server, client->fd);
        return ESP_FAIL;
    }

    if (sent > 0) {
        size_t offset = (size_t)sent;
        int64_t deadline_us = esp_timer_get_time() + (WS_PARTIAL_SEND_TIMEOUT_MS * 1000LL);
        while (offset < total) {
            int64_t rem_us = deadline_us - esp_timer_get_time();
            if (rem_us <= 0) break;

            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(client->fd, &wfds);
            struct timeval tv = {
                .tv_sec = (time_t)(rem_us / 1000000LL),
                .tv_usec = (suseconds_t)(rem_us % 1000000LL),
            };
            int ready = select(client->fd + 1, NULL, &wfds, NULL, &tv);
            if (ready < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ready == 0) break;

            int n = httpd_socket_send(
                client->server, client->fd, frame + offset, total - offset, MSG_DONTWAIT
            );
            if (n > 0) {
                offset += (size_t)n;
            } else if (n != HTTPD_SOCK_ERR_TIMEOUT) {
                break;
            }
        }
        free(frame);
        if (offset == total) {
            client->stall_since_ms = 0;
            return ESP_OK;
        }
        ESP_LOGW(
            TAG,
            "Client fd %d not keeping up (partial send %u/%u), disconnecting",
            client->fd,
            (unsigned)offset,
            (unsigned)total
        );
        httpd_sess_trigger_close(client->server, client->fd);
        return ESP_FAIL;
    }

    free(frame);
    ESP_LOGW(TAG, "Client fd %d send failed (%d), disconnecting", client->fd, sent);
    httpd_sess_trigger_close(client->server, client->fd);
    return ESP_FAIL;
}

/**
 * Runs on the httpd task: fan a serialized broadcast out to every subscribed client.
 */
static void _broadcast_work(void* arg) {
    ws_broadcast_job_t* job = (ws_broadcast_job_t*)arg;

    websocket_client_t* client = NULL;
    list_foreach(_client_list, client) {
        if ((client->broadcast_flags & job->broadcast_flags) && _is_websocket(client)) {
            _send_frame_nonblocking(client, job->payload, job->len);
        }
    }

    _lock();
    if (_pending_jobs > 0) _pending_jobs--;
    if (job->broadcast_flags & WS_BROADCAST_READINGS) _readings_pending = false;
    _unlock();

    cJSON_free(job->payload);
    free(job);
}

/**
 * Queue a broadcast. Safe to call from any task and never blocks on the network: the actual
 * sends happen on the httpd task. Readings are dropped while a previous readings broadcast is
 * still pending (they are superseded by the next one anyway).
 */
esp_err_t websocket_broadcast(cJSON* root, int broadcast_flags) {
    bool readings = (broadcast_flags & WS_BROADCAST_READINGS) != 0;

    _lock();
    bool skip = _server == NULL || _pending_jobs >= WS_BROADCAST_MAX_PENDING ||
                (readings && _readings_pending);
    _unlock();
    if (skip) return ESP_OK;

    char* str = cJSON_PrintUnformatted(root);
    if (str == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (!cJSON_HasObjectItem(root, "readings")) {
        ESP_LOGD(TAG, "Broadcasting: %s", str);
    }

    ws_broadcast_job_t* job = malloc(sizeof(ws_broadcast_job_t));
    if (job == NULL) {
        cJSON_free(str);
        return ESP_ERR_NO_MEM;
    }

    job->payload = str;
    job->len = strlen(str);
    job->broadcast_flags = broadcast_flags;

    esp_err_t err = ESP_FAIL;

    _lock();
    if (_server != NULL) {
        err = httpd_queue_work(_server, _broadcast_work, job);
        if (err == ESP_OK) {
            _pending_jobs++;
            if (readings) _readings_pending = true;
        }
    }
    _unlock();

    if (err != ESP_OK) {
        cJSON_free(str);
        free(job);
    }

    return ESP_OK;
}

static void _json_add_error(cJSON* root, const char* error) {
    const char* err_key = "error";
    if (root == NULL || !cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Attempt to add error to non-object: %s", error);
        return;
    }

    cJSON* err_obj = cJSON_GetObjectItem(root, err_key);

    if (err_obj == NULL) {
        cJSON_AddStringToObject(root, err_key, error);
        return;
    }

    if (!cJSON_IsArray(err_obj)) {
        cJSON* err_array = cJSON_CreateArray();
        cJSON_ReplaceItemViaPointer(root, err_obj, err_array);
        cJSON_AddItemToArray(err_array, err_obj);
    }

    cJSON* err_item = cJSON_CreateString(error);
    cJSON_AddItemToArray(err_obj, err_item);
}

void websocket_register_command(const websocket_command_t* command) {
    list_node_t* node = list_add(&_cmd_list, command);
    if (node == NULL) {
        ESP_LOGE(TAG, "Command registration failed, NO MEM!");
    } else {
        ESP_LOGD(TAG, "* Command: %s", command->command);
    }
}

void websocket_run_command(
    const char* command, cJSON* data, cJSON* response, websocket_client_t* client
) {
    ESP_LOGD(TAG, "Running command: %s", command);

    websocket_command_t* cmd = NULL;
    list_foreach(_cmd_list, cmd) {
        ESP_LOGD(TAG, "Checking %s", cmd->command);
        if (!strcmp(command, cmd->command)) {
            if (!update_mode_try_work()) {
                _json_add_error(response, "Firmware update in progress");
                return;
            }
            command_err_t err = cmd->func(data, response, client);
            update_mode_end_work();
            if (err != CMD_OK) {
                cJSON_AddNumberToObject(response, "errno", err);
            }
            return;
        }
    }

    _json_add_error(response, "NOT IMPLEMENTED");
}

void websocket_run_commands(cJSON* commands, cJSON* response, websocket_client_t* client) {
    cJSON* el = NULL;
    char* key = NULL;

    cJSON_ArrayForEach(el, commands) {
        key = el->string;

        if (key != NULL) {
            cJSON* nonce = cJSON_GetObjectItem(el, "nonce");
            cJSON* cmd_rsp = cJSON_CreateObject();

            if (nonce != NULL) {
                cJSON_AddNumberToObject(cmd_rsp, "nonce", nonce->valueint);
            }

            websocket_run_command(key, el, cmd_rsp, client);

            if (cJSON_GetArraySize(cmd_rsp) > 0) {
                cJSON_AddItemToObject(response, key, cmd_rsp);
            } else {
                cJSON_Delete(cmd_rsp);
            }
        }
    }
}

esp_err_t websocket_open_fd(httpd_handle_t hd, int sockfd) {
    char ipstr[INET6_ADDRSTRLEN];
    struct sockaddr_in6 addr;
    socklen_t addr_size = sizeof(addr);
    if (getpeername(sockfd, (struct sockaddr*)&addr, &addr_size) < 0) {
        ipstr[0] = '\0';
    } else {
        inet_ntop(AF_INET6, &addr.sin6_addr, ipstr, sizeof(ipstr));
    }

    ESP_LOGI(TAG, "websocket_open_fd(hd: %p, sockfd: %d) => IP: %s", hd, sockfd, ipstr);

    int keepalive = 1, idle = WS_KEEPALIVE_IDLE_S, interval = WS_KEEPALIVE_INTERVAL_S,
        count = WS_KEEPALIVE_COUNT, nodelay = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    websocket_client_t* client = malloc(sizeof(websocket_client_t));
    if (client == NULL) {
        ESP_LOGE(TAG, "websocket_open_fd: out of memory");
        return ESP_ERR_NO_MEM;
    }
    client->fd = sockfd;
    client->server = hd;
    client->broadcast_flags = 0;
    client->stall_since_ms = 0;
    list_add(&_client_list, client);
    // Let ESP-IDF handle cleanup of client context on socket close:
    httpd_sess_set_ctx(hd, sockfd, client, free);
    return ESP_OK;
}

void websocket_close_fd(httpd_handle_t hd, int sockfd) {
    ESP_LOGI(TAG, "websocket_close_fd(hd: %p, sockfd: %d)", hd, sockfd);
    websocket_client_t* client = NULL;
    list_foreach(_client_list, client) {
        if (client->fd == sockfd) {
            list_remove(&_client_list, client);
            break;
        }
    }

    // When a close_fn is installed, esp_http_server does NOT close the socket itself (see
    // httpd_sess_delete); without this every finished session leaked an lwIP socket until
    // accept() failed with ENFILE and the server stopped responding.
    close(sockfd);
}

esp_err_t websocket_handler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        ESP_LOGD(TAG, "This was the handshake.");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t* buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Calculate frame length:
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Got frame, length: %d", ws_pkt.len);
    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }

        // Actually receive frame:
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            goto cleanup;
        }

        ESP_LOGI(TAG, "Got packet type 0x%02x", ws_pkt.type);
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "Client requesting CLOSE");
    } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        ESP_LOGI(TAG, "Received PING message");
    } else if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        ESP_LOGI(TAG, "Received PONG message");
    } else if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGI(TAG, "data: %s", ws_pkt.payload);
        httpd_ws_frame_t resp_pkt;
        memset(&resp_pkt, 0, sizeof(httpd_ws_frame_t));
        resp_pkt.type = HTTPD_WS_TYPE_TEXT;
        resp_pkt.final = true;

        cJSON* command = cJSON_Parse((char*)ws_pkt.payload);
        cJSON* response = cJSON_CreateObject();

        if (command == NULL) {
            _json_add_error(response, cJSON_GetErrorPtr());
        } else {
            if (req->sess_ctx == NULL) {
                ESP_LOGE(TAG, "Request session context was NULL!");
            } else {
                websocket_run_commands(command, response, (websocket_client_t*)req->sess_ctx);
            }
        }

        if (cJSON_GetArraySize(response) > 0) {
            resp_pkt.payload = (uint8_t*)cJSON_PrintUnformatted(response);
            resp_pkt.len = strlen((char*)resp_pkt.payload);

            ESP_LOGI(TAG, "Transmitting response: %s", resp_pkt.payload);
            ret = httpd_ws_send_frame(req, &resp_pkt);
        }

        if (command != NULL) cJSON_Delete(command);

        cJSON_Delete(response);
        cJSON_free(resp_pkt.payload);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send frame: %s", esp_err_to_name(ret));
        }
    }

cleanup:
    if (buf != NULL) free(buf);
    return ret;
}

esp_err_t websocket_connect_to_bridge(const char* address, int port) {
    return ESP_FAIL;
}