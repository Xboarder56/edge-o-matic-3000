#include "drivers/plugin_driver.h"
#include "actions/http.h"
#include "mt_actions_internal.h"
#include "system/action_manager.h"
#include "system/event_manager.h"
#include <esp_log.h>
#include <esp_timer.h>
#include "freertos/queue.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include <string.h>

static const char* TAG = "plugin_driver";

static int on_tx_complete(
    uint16_t conn_handle, const struct ble_gatt_error* error, struct ble_gatt_attr* attr, void* arg
) {
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)arg;
    if (!ctx) return 0;

    if (error->status != 0) {
        ESP_LOGW(TAG, "TX failed: status=%d", error->status);
    }

    ctx->pending_tx_flag = false;
    ctx->tx_retry_count = 0;
    return 0;
}

static void ctx_flush_tx(ble_conn_ctx_t* ctx) {
    if (!ctx || ctx->pending_len == 0 || ctx->pending_tx_flag) return;

    // EBUSY backoff: don't hammer NimBLE while a prior GATT op is finishing
    if (ctx->retry_after && xTaskGetTickCount() < ctx->retry_after) return;

    if (xSemaphoreTake(ctx->tx_mutex, 1000UL / portTICK_RATE_MS)) {
        char buffer[PLUGIN_DRIVER_TX_MAX];
        int len = ctx->pending_len;
        bool use_no_rsp = ctx->use_write_no_rsp;
        uint16_t handle = ctx->pending_handle;

        memcpy(buffer, ctx->pending_tx, len);
        buffer[len] = '\0';

        ctx->pending_len = 0;
        ctx->pending_handle = 0;
        xSemaphoreGive(ctx->tx_mutex);

        int rc;
        if (handle != 0) {
            // Characteristic chosen with ble_use_characteristic().
            bool can_no_rsp = ctx->tx_select_props & BLE_GATT_CHR_PROP_WRITE_NO_RSP;
            bool can_rsp = ctx->tx_select_props & BLE_GATT_CHR_PROP_WRITE;

            if ((use_no_rsp && can_no_rsp) || !can_rsp) {
                use_no_rsp = true;
                rc = ble_gattc_write_no_rsp_flat(ctx->peer->conn_handle, handle, buffer, len);
            } else {
                use_no_rsp = false;
                ctx->pending_tx_flag = true;
                rc = ble_gattc_write_flat(
                    ctx->peer->conn_handle, handle, buffer, len, on_tx_complete, ctx
                );
            }
        } else if (use_no_rsp && ctx->has_tx_no_rsp_chr) {
            rc = ble_gattc_write_no_rsp_flat(
                ctx->peer->conn_handle, ctx->tx_no_rsp_chr.val_handle, buffer, len
            );
        } else if (ctx->has_tx_chr) {
            ctx->pending_tx_flag = true;
            rc = ble_gattc_write_flat(
                ctx->peer->conn_handle, ctx->tx_chr.val_handle, buffer, len, on_tx_complete, ctx
            );
        } else {
            ESP_LOGW(TAG, "flush: no suitable write characteristic");
            return;
        }

        if (rc == 0) {
            ctx->retry_after = 0;
            ctx->tx_retry_count = 0;
            if (use_no_rsp) ctx->pending_tx_flag = false;

            if (handle != 0) {
                ESP_LOGD(TAG, "TX: %d bytes to handle %u", len, handle);
            } else {
                ESP_LOGI(TAG, "TX: %.*s", len, buffer);
            }
        } else if (rc == 7 /* BLE_HS_EBUSY */) {
            ctx->pending_tx_flag = false;
            ctx->tx_retry_count++;

            if (ctx->tx_retry_count <= PLUGIN_DRIVER_TX_MAX_RETRIES) {
                if (xSemaphoreTake(ctx->tx_mutex, pdMS_TO_TICKS(10))) {
                    if (ctx->pending_len == 0) {
                        memcpy(ctx->pending_tx, buffer, len);
                        ctx->pending_len = len;
                        ctx->use_write_no_rsp = use_no_rsp;
                        ctx->pending_handle = handle;
                    }

                    xSemaphoreGive(ctx->tx_mutex);
                }

                ctx->retry_after = xTaskGetTickCount() + pdMS_TO_TICKS(50);
                ESP_LOGD(
                    TAG, "BLE busy, retry %d/%d", ctx->tx_retry_count, PLUGIN_DRIVER_TX_MAX_RETRIES
                );
            } else {
                ESP_LOGW(TAG, "Write dropped after %d retries (BLE busy)", ctx->tx_retry_count);
                ctx->tx_retry_count = 0;
                ctx->retry_after = 0;
            }
        } else {
            ESP_LOGW(TAG, "Write failed: rc=%d", rc);
            ctx->pending_tx_flag = false;
        }
    } else {
        ESP_LOGW(TAG, "flush: timeout waiting for TX mutex");
    }
}

/**
 * @brief Queue data into the BLE TX buffer
 *
 * Routes through the async TX pipe. The wait_response parameter selects
 * between write-with-response and write-no-response GATT semantics.
 */
static int ble_io_write(void* device, const void* data, size_t len, bool wait_response) {
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)device;
    if (!ctx || !ctx->peer) return -1;

    if (wait_response && !ctx->has_tx_chr) return -1;
    if (!wait_response && !ctx->has_tx_no_rsp_chr && !ctx->has_tx_chr) return -1;

    if ((int)len >= PLUGIN_DRIVER_TX_MAX) {
        ESP_LOGW(TAG, "ble_io_write: data too long (%d >= %d)", (int)len, PLUGIN_DRIVER_TX_MAX);
        len = PLUGIN_DRIVER_TX_MAX - 1;
    }

    if (xSemaphoreTake(ctx->tx_mutex, 1000UL / portTICK_RATE_MS)) {
        memcpy(ctx->pending_tx, data, len);
        ctx->pending_tx[len] = '\0';
        ctx->pending_len = (int)len;
        ctx->use_write_no_rsp = false;
        xSemaphoreGive(ctx->tx_mutex);
        return 0;
    }

    ESP_LOGW(TAG, "ble_io_write: timeout waiting for TX mutex");
    return -1;
}

/**
 * @brief Free the backend connector
 */
static void ble_io_close(void* device) {
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)device;
    if (!ctx) return;

    ESP_LOGI(TAG, "Closing BLE context for '%s'", ctx->peer ? ctx->peer->name : "?");

    if (ctx->peer) {
        ctx->peer->driver_state = NULL;
    }

    if (ctx->tx_mutex) {
        vSemaphoreDelete(ctx->tx_mutex);
    }

    free(ctx);
}

/**
 * @brief Static I/O backend vtable for BLE connections
 */
static const mta_io_backend_t _ble_io_backend = {
    .write = ble_io_write,
    .read = NULL,
    .close = ble_io_close,
};

static void plugin_driver_event_handler(
    const char* event, void* event_arg_ptr, int event_arg_int, void* handler_arg
) {
    (void)event_arg_ptr;
    (void)handler_arg;

    // Map firmware event names to plugin event names
    const char* plugin_event = NULL;

    if (strcmp(event, "EVT_SPEED_CHANGE") == 0) {
        plugin_event = "speed_change";
    } else if (strcmp(event, "EVT_AROUSAL_CHANGE") == 0) {
        plugin_event = "arousal_change";
    }

    if (!plugin_event) return;

    mta_driver_instance_dispatch_all(plugin_event, event_arg_int);
}

void plugin_driver_init(void) {
    ESP_LOGI(TAG, "Plugin driver subsystem initialized");
    event_manager_register_handler(EVT_SPEED_CHANGE, plugin_driver_event_handler, NULL);
    event_manager_register_handler(EVT_AROUSAL_CHANGE, plugin_driver_event_handler, NULL);
}

bool plugin_driver_try_claim(peer_t* peer) {
    size_t plugin_count = action_manager_get_plugin_count();

    for (size_t i = 0; i < plugin_count; i++) {
        mta_plugin_t* plugin = action_manager_get_plugin(i);
        if (!plugin) continue;

        const char* type = mta_plugin_get_type(plugin);
        if (!type || strcmp(type, "ble_driver") != 0) continue;

        // Check if this peer already has an active driver instance
        if (peer->driver_state != NULL) continue;

        // Check BLE name match
        const char* ble_name = mta_plugin_get_match_ble_name(plugin);
        const char* ble_prefix = mta_plugin_get_match_ble_name_prefix(plugin);
        bool matched = false;

        if (ble_name && strcmp(peer->name, ble_name) == 0) {
            matched = true;
        } else if (ble_prefix && strncmp(peer->name, ble_prefix, strlen(ble_prefix)) == 0) {
            matched = true;
        }

        if (!matched) continue;

        ESP_LOGI(
            TAG,
            "Plugin '%s' matched peer '%s'",
            mta_plugin_get_name(plugin) ? mta_plugin_get_name(plugin) : "?",
            peer->name
        );

        // GATT service discovery
        int rc = bluetooth_manager_discover_all(peer);
        if (rc != 0) {
            ESP_LOGW(TAG, "Service discovery failed: rc=%d", rc);
            return false;
        }

        // Scan for write characteristics
        struct ble_gatt_chr* write_chr = NULL;
        struct ble_gatt_chr* write_no_rsp_chr = NULL;

        struct peer_svc_node* svc = peer->svcs;
        while (svc != NULL) {
            struct peer_chr_node* chr = svc->chrs;
            while (chr != NULL) {
                uint8_t prop = chr->chr.properties;
                if ((prop & BLE_GATT_CHR_PROP_WRITE) && !write_chr) {
                    write_chr = &chr->chr;
                }
                if ((prop & BLE_GATT_CHR_PROP_WRITE_NO_RSP) && !write_no_rsp_chr) {
                    write_no_rsp_chr = &chr->chr;
                }
                chr = chr->next;
            }
            svc = svc->next;
        }

        if (!write_chr && !write_no_rsp_chr) {
            ESP_LOGW(TAG, "No writable characteristics found for '%s'", peer->name);
            return false;
        }

        // Allocate BLE connection context
        ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)calloc(1, sizeof(ble_conn_ctx_t));
        if (!ctx) return false;

        ctx->peer = peer;
        ctx->tx_mutex = xSemaphoreCreateMutex();
        ctx->pending_tx_flag = false;
        ctx->pending_len = 0;
        ctx->retry_after = 0;
        ctx->tx_retry_count = 0;

        if (write_chr) {
            ctx->tx_chr = *write_chr;
            ctx->has_tx_chr = true;
        }
        if (write_no_rsp_chr) {
            ctx->tx_no_rsp_chr = *write_no_rsp_chr;
            ctx->has_tx_no_rsp_chr = true;
        }

        // Create driver instance: device = ctx so I/O backend callbacks
        // receive the ble_conn_ctx_t directly. The peer_t* is reachable
        // via ctx->peer, and peer->driver_state points back to the instance.
        mta_driver_instance_t* instance = mta_driver_instance_create(plugin, ctx, &_ble_io_backend);
        if (!instance) {
            vSemaphoreDelete(ctx->tx_mutex);
            free(ctx);
            return false;
        }

        // Set scope user_data to the ble_conn_ctx_t so bleWrite can find the TX pipe
        mta_scope_set_user_data(mta_driver_instance_get_scope(instance), ctx);

        // Cross-link: peer -> driver instance
        peer->driver_state = (void*)instance;

        // Register in global instance list
        mta_driver_instance_add(instance);

        // Fire connect event
        mta_driver_instance_dispatch(instance, "connect", 0, NULL);

        ESP_LOGI(
            TAG,
            "Connection bound: write=%s, write_no_rsp=%s",
            write_chr ? "yes" : "no",
            write_no_rsp_chr ? "yes" : "no"
        );

        return true;
    }

    return false;
}

void plugin_driver_release_peer(peer_t* peer) {
    if (!peer) return;

    mta_driver_instance_t* instance = (mta_driver_instance_t*)peer->driver_state;
    if (!instance) {
        ESP_LOGW(TAG, "release_peer: peer '%s' has no driver instance", peer->name);
        return;
    }

    ESP_LOGI(TAG, "Releasing connection for '%s'", peer->name);

    // remove from global list first, then destroy (fires disconnect, closes I/O, frees scope)
    mta_driver_instance_remove(instance);
    mta_driver_instance_destroy(instance);
}

static void ctx_start_op(ble_conn_ctx_t* ctx);
static void rx_deliver_all(void);

static void tick_flush_cb(mta_driver_instance_t* instance, void* arg) {
    (void)arg;
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)mta_driver_instance_get_device(instance);
    if (!ctx) return;
    ctx_flush_tx(ctx);
    ctx_start_op(ctx);
}

typedef struct {
    int64_t now_ms;
} tick_dispatch_ctx_t;

static void tick_dispatch_cb(mta_driver_instance_t* instance, void* arg) {
    tick_dispatch_ctx_t* tctx = (tick_dispatch_ctx_t*)arg;
    mta_driver_instance_dispatch(instance, "tick", (int)tctx->now_ms, NULL);
}

void plugin_driver_tick(void) {
    // Deliver a finished plugin HTTP request made by a driver instance.
    action_http_poll(true);

    // Flush pending BLE TX for all active driver connections.
    mta_driver_instance_foreach(tick_flush_cb, NULL);

    // Deliver reads, notifications and procedure results (ble_read, ble_notify, ...).
    rx_deliver_all();

    // Dispatch "tick" to all driver instances so time-based plugin logic can run.
    // $arg is milliseconds since boot (wraps at ~24 days; honestly if someone has
    // been edging for that long, you should probably check in on them. Make sure
    // they're drinking water.
    //
    // TODO: Promote "tick" to a built-in mt-actions platform event so that any
    // host embedding mt-actions dispatches it automatically without per-product
    // boilerplate.
    tick_dispatch_ctx_t tctx = { .now_ms = esp_timer_get_time() / 1000LL };
    mta_driver_instance_foreach(tick_dispatch_cb, &tctx);
}

typedef struct {
    plugin_driver_enum_cb_t cb;
    void* arg;
} enum_ctx_t;

static void enum_cb(mta_driver_instance_t* instance, void* arg) {
    enum_ctx_t* ectx = (enum_ctx_t*)arg;
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)mta_driver_instance_get_device(instance);
    if (ctx && ctx->peer) ectx->cb(ctx->peer, ectx->arg);
}

void plugin_driver_enumerate(plugin_driver_enum_cb_t cb, void* arg) {
    if (!cb) return;
    enum_ctx_t ectx = { .cb = cb, .arg = arg };
    mta_driver_instance_foreach(enum_cb, &ectx);
}

bool plugin_driver_can_handle_name(const char* name) {
    if (!name) return false;

    size_t plugin_count = action_manager_get_plugin_count();

    for (size_t i = 0; i < plugin_count; i++) {
        mta_plugin_t* plugin = action_manager_get_plugin(i);
        if (!plugin) continue;

        const char* type = mta_plugin_get_type(plugin);
        if (!type || strcmp(type, "ble_driver") != 0) continue;

        const char* ble_name = mta_plugin_get_match_ble_name(plugin);
        const char* ble_prefix = mta_plugin_get_match_ble_name_prefix(plugin);

        if (ble_name && strcmp(name, ble_name) == 0) return true;
        if (ble_prefix && strncmp(name, ble_prefix, strlen(ble_prefix)) == 0) return true;
    }

    return false;
}

// =============================================================================================
// Opt-in GATT extensions: characteristic selection, reads, notifications, encryption, MTU.
//
// NimBLE callbacks run on the NimBLE host task. They never call into mt-actions: results are
// queued and delivered as plugin events from plugin_driver_tick() on the plugin task, the same
// task that runs every other driver event.
// =============================================================================================

typedef enum {
    RX_READ = 1,
    RX_NOTIFY,
    RX_SUBSCRIBED,
    RX_SECURED,
    RX_MTU,
} rx_kind_t;

typedef struct {
    uint16_t conn_handle;
    uint16_t attr_handle;
    int16_t value; // >= 0 result (length, MTU, 0 = ok) or negative error
    uint8_t kind;  // rx_kind_t
    uint8_t len;
    uint8_t data[PLUGIN_DRIVER_RX_MAX];
} rx_item_t;

#define RX_QUEUE_LEN 6

static QueueHandle_t _rx_queue = NULL;
static rx_item_t* _rx_item = NULL;              // item being delivered (plugin task only)
static const rx_item_t* _rx_current = NULL;      // non-NULL during delivery
static ble_conn_ctx_t* _rx_current_ctx = NULL;

// Created on first use by a plugin, so devices without such plugins spend nothing on it.
static bool rx_queue_ensure(void) {
    if (_rx_item == NULL) _rx_item = calloc(1, sizeof(rx_item_t));
    if (_rx_item != NULL && _rx_queue == NULL) {
        _rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_item_t));
    }
    return _rx_queue != NULL;
}

static void rx_push(
    uint16_t conn_handle, rx_kind_t kind, uint16_t attr_handle, int value, const uint8_t* data,
    int len
) {
    if (_rx_queue == NULL) return;

    rx_item_t item = {
        .conn_handle = conn_handle,
        .attr_handle = attr_handle,
        .value = (int16_t)(value < -32768 ? -32768 : value > 32767 ? 32767 : value),
        .kind = kind,
        .len = 0,
    };

    if (data != NULL && len > 0) {
        item.len = len > PLUGIN_DRIVER_RX_MAX ? PLUGIN_DRIVER_RX_MAX : len;
        memcpy(item.data, data, item.len);
    }

    if (xQueueSend(_rx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full, dropped %s", kind == RX_NOTIFY ? "notification" : "result");
    }
}

static int nimble_err(int status) {
    return status > 0 ? -status : (status == 0 ? -1 : status);
}

// --- UUIDs -----------------------------------------------------------------------------------

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 0000xxxx-0000-1000-8000-00805f9b34fb, little-endian, without the 32-bit part (bytes 12..15).
static const uint8_t BT_BASE_UUID_LE[12] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00,
};

// Express Bluetooth-base 128-bit UUIDs as 16/32-bit so either spelling matches.
static void uuid_normalize(ble_uuid_any_t* u) {
    if (u->u.type != BLE_UUID_TYPE_128) return;
    if (memcmp(u->u128.value, BT_BASE_UUID_LE, sizeof(BT_BASE_UUID_LE)) != 0) return;

    uint32_t v = u->u128.value[12] | (u->u128.value[13] << 8) | (u->u128.value[14] << 16) |
                 ((uint32_t)u->u128.value[15] << 24);

    if (v <= 0xffff) {
        u->u16.u.type = BLE_UUID_TYPE_16;
        u->u16.value = (uint16_t)v;
    } else {
        u->u32.u.type = BLE_UUID_TYPE_32;
        u->u32.value = v;
    }
}

static bool uuid_parse(const char* s, ble_uuid_any_t* out) {
    if (s == NULL) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

    size_t n = strlen(s);

    if (n == 4 || n == 8) {
        uint32_t v = 0;
        for (size_t i = 0; i < n; i++) {
            int h = hexval(s[i]);
            if (h < 0) return false;
            v = (v << 4) | h;
        }

        if (n == 4) {
            out->u16.u.type = BLE_UUID_TYPE_16;
            out->u16.value = (uint16_t)v;
        } else {
            out->u32.u.type = BLE_UUID_TYPE_32;
            out->u32.value = v;
        }
        return true;
    }

    if (n == 36) {
        uint8_t be[16];
        int k = 0;

        for (size_t i = 0; i < n; i++) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (s[i] != '-') return false;
                continue;
            }
            int hi = hexval(s[i]);
            int lo = hexval(s[++i]);
            if (hi < 0 || lo < 0 || k >= 16) return false;
            be[k++] = (hi << 4) | lo;
        }

        if (k != 16) return false;
        out->u128.u.type = BLE_UUID_TYPE_128;
        for (int i = 0; i < 16; i++) out->u128.value[i] = be[15 - i];
        uuid_normalize(out);
        return true;
    }

    return false;
}

const struct ble_gatt_chr* plugin_driver_find_chr(
    ble_conn_ctx_t* ctx, const char* uuid_text, uint16_t* end_handle
) {
    if (ctx == NULL || ctx->peer == NULL) return NULL;

    ble_uuid_any_t want;
    if (!uuid_parse(uuid_text, &want)) return NULL;

    for (struct peer_svc_node* svc = ctx->peer->svcs; svc != NULL; svc = svc->next) {
        for (struct peer_chr_node* chr = svc->chrs; chr != NULL; chr = chr->next) {
            ble_uuid_any_t have = chr->chr.uuid;
            uuid_normalize(&have);
            if (ble_uuid_cmp(&want.u, &have.u) != 0) continue;

            if (end_handle != NULL) {
                // Descriptors sit between this value handle and the next characteristic.
                *end_handle = (chr->next != NULL) ? chr->next->chr.def_handle - 1
                                                  : svc->svc.end_handle;
            }
            return &chr->chr;
        }
    }

    return NULL;
}

static const struct ble_gatt_chr* find_chr_by_handle(ble_conn_ctx_t* ctx, uint16_t val_handle) {
    if (ctx == NULL || ctx->peer == NULL) return NULL;

    for (struct peer_svc_node* svc = ctx->peer->svcs; svc != NULL; svc = svc->next) {
        for (struct peer_chr_node* chr = svc->chrs; chr != NULL; chr = chr->next) {
            if (chr->chr.val_handle == val_handle) return &chr->chr;
        }
    }

    return NULL;
}

const char* plugin_driver_rx_uuid(ble_conn_ctx_t* ctx, char* buf) {
    int len;
    uint16_t attr;
    buf[0] = '\0';
    if (plugin_driver_rx_data(ctx, &len, &attr) == NULL) return buf;

    const struct ble_gatt_chr* chr = find_chr_by_handle(ctx, attr);
    if (chr != NULL) ble_uuid_to_str(&chr->uuid.u, buf);
    return buf;
}

// --- GATT procedures (callbacks run on the NimBLE host task) --------------------------------

static int on_read(
    uint16_t conn_handle, const struct ble_gatt_error* error, struct ble_gatt_attr* attr, void* arg
) {
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)arg;

    if (error->status == 0 && attr != NULL) {
        int room = PLUGIN_DRIVER_RX_MAX - ctx->read_len;
        int len = OS_MBUF_PKTLEN(attr->om);
        if (len > room) len = room;
        if (len > 0) os_mbuf_copydata(attr->om, 0, len, ctx->read_buf + ctx->read_len);
        ctx->read_len += len;
        return 0; // keep reading (long read)
    }

    if (error->status == BLE_HS_EDONE) {
        rx_push(conn_handle, RX_READ, ctx->op_handle, ctx->read_len, ctx->read_buf, ctx->read_len);
    } else {
        rx_push(conn_handle, RX_READ, ctx->op_handle, nimble_err(error->status), NULL, 0);
    }

    ctx->op_running = BLE_OP_NONE;
    return 0;
}

static int on_cccd_written(
    uint16_t conn_handle, const struct ble_gatt_error* error, struct ble_gatt_attr* attr, void* arg
) {
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)arg;

    if (error->status == 0) {
        if (ctx->op_cccd_value != 0) ctx->rx_enabled = true;
        rx_push(conn_handle, RX_SUBSCRIBED, ctx->op_handle, 0, NULL, 0);
    } else {
        rx_push(conn_handle, RX_SUBSCRIBED, ctx->op_handle, nimble_err(error->status), NULL, 0);
    }

    ctx->op_running = BLE_OP_NONE;
    return 0;
}

static int on_dsc(
    uint16_t conn_handle, const struct ble_gatt_error* error, uint16_t chr_val_handle,
    const struct ble_gatt_dsc* dsc, void* arg
) {
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)arg;

    if (error->status == 0 && dsc != NULL) {
        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            ctx->op_cccd_handle = dsc->handle;
        }
        return 0;
    }

    int rc = error->status;

    if (rc == BLE_HS_EDONE) {
        if (ctx->op_cccd_handle == 0) {
            rc = BLE_HS_ENOENT;
        } else {
            uint8_t value[2] = { ctx->op_cccd_value & 0xff, ctx->op_cccd_value >> 8 };
            rc = ble_gattc_write_flat(
                conn_handle, ctx->op_cccd_handle, value, sizeof(value), on_cccd_written, ctx
            );
            if (rc == 0) return 0; // on_cccd_written finishes the procedure
        }
    }

    ESP_LOGW(TAG, "Subscribe failed: rc=%d", rc);
    rx_push(conn_handle, RX_SUBSCRIBED, ctx->op_handle, nimble_err(rc), NULL, 0);
    ctx->op_running = BLE_OP_NONE;
    return 0;
}

static int on_mtu(uint16_t conn_handle, const struct ble_gatt_error* error, uint16_t mtu, void* arg) {
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)arg;
    rx_push(conn_handle, RX_MTU, 0, error->status == 0 ? mtu : nimble_err(error->status), NULL, 0);
    ctx->op_running = BLE_OP_NONE;
    return 0;
}

int plugin_driver_request_op(ble_conn_ctx_t* ctx, ble_op_t op, uint16_t handle, uint16_t cccd_value) {
    if (ctx == NULL || !rx_queue_ensure()) return -1;
    if (ctx->op_requested != BLE_OP_NONE || ctx->op_running != BLE_OP_NONE) return -1;

    ctx->op_handle = handle;
    ctx->op_cccd_value = cccd_value;
    ctx->op_requested = op;
    return 0;
}

// Runs on the plugin task from plugin_driver_tick().
static void ctx_start_op(ble_conn_ctx_t* ctx) {
    if (ctx->op_requested == BLE_OP_NONE || ctx->op_running != BLE_OP_NONE) return;
    if (ctx->pending_tx_flag) return; // a write with response is in flight; start after it

    uint16_t conn = ctx->peer->conn_handle;
    ble_op_t op = ctx->op_requested;
    rx_kind_t kind = op == BLE_OP_READ ? RX_READ : op == BLE_OP_SUBSCRIBE ? RX_SUBSCRIBED : RX_MTU;
    int rc = 0;

    ctx->op_requested = BLE_OP_NONE;
    ctx->op_running = op;

    switch (op) {
    case BLE_OP_READ:
        ctx->read_len = 0;
        rc = ble_gattc_read_long(conn, ctx->op_handle, 0, on_read, ctx);
        break;

    case BLE_OP_SUBSCRIBE:
        ctx->op_cccd_handle = 0;
        rc = ble_gattc_disc_all_dscs(conn, ctx->op_handle, ctx->op_end_handle, on_dsc, ctx);
        break;

    case BLE_OP_MTU:
        rc = ble_gattc_exchange_mtu(conn, on_mtu, ctx);
        if (rc == BLE_HS_EALREADY) {
            // Already exchanged on this connection: report the current value.
            ctx->op_running = BLE_OP_NONE;
            rx_push(conn, RX_MTU, 0, ble_att_mtu(conn), NULL, 0);
            return;
        }
        break;

    default: ctx->op_running = BLE_OP_NONE; return;
    }

    if (rc == BLE_HS_EBUSY) {
        // Another ATT procedure is outstanding: try again on the next tick.
        ctx->op_running = BLE_OP_NONE;
        ctx->op_requested = op;
    } else if (rc != 0) {
        ctx->op_running = BLE_OP_NONE;
        rx_push(conn, kind, ctx->op_handle, nimble_err(rc), NULL, 0);
    }
}

// --- Encryption ------------------------------------------------------------------------------

int plugin_driver_secure(ble_conn_ctx_t* ctx) {
    if (ctx == NULL || ctx->peer == NULL || !rx_queue_ensure()) return -1;

    uint16_t conn = ctx->peer->conn_handle;
    struct ble_gap_conn_desc desc;

    if (ble_gap_conn_find(conn, &desc) == 0 && desc.sec_state.encrypted) {
        rx_push(conn, RX_SECURED, 0, 0, NULL, 0);
        return 0;
    }

    // Bond with LE Secure Connections, "Just Works" (the EOM has no way to show or enter a
    // passkey). Set here, not at boot, so devices that never ask for this pair exactly as before.
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ctx->secure_requested = true;
    int rc = ble_gap_security_initiate(conn);

    if (rc == 0 || rc == BLE_HS_EALREADY) return 0; // result arrives via ENC_CHANGE

    ctx->secure_requested = false;
    return nimble_err(rc);
}

static ble_conn_ctx_t* peer_ctx(peer_t* peer) {
    if (peer == NULL || peer->driver_state == NULL) return NULL;
    return (ble_conn_ctx_t*)mta_driver_instance_get_device((mta_driver_instance_t*)peer->driver_state);
}

void plugin_driver_on_enc_change(peer_t* peer, int status) {
    ble_conn_ctx_t* ctx = peer_ctx(peer);

    if (status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)) {
        // The peer forgot our bond (e.g. it was reset or revoked us). Drop ours as well so the
        // next ble_secure() pairs from scratch instead of failing the same way again.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(peer->conn_handle, &desc) == 0) {
            ESP_LOGW(TAG, "Peer '%s' lost its bond; deleting ours", peer->name);
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
    }

    if (ctx == NULL || !ctx->secure_requested) return;
    ctx->secure_requested = false;
    rx_push(peer->conn_handle, RX_SECURED, 0, status == 0 ? 0 : nimble_err(status), NULL, 0);
}

// --- Notifications ---------------------------------------------------------------------------

void plugin_driver_on_notify(peer_t* peer, uint16_t attr_handle, struct os_mbuf* om) {
    ble_conn_ctx_t* ctx = peer_ctx(peer);
    if (ctx == NULL || !ctx->rx_enabled || _rx_queue == NULL || om == NULL) return;

    uint8_t data[PLUGIN_DRIVER_RX_MAX];
    int len = OS_MBUF_PKTLEN(om);
    if (len > PLUGIN_DRIVER_RX_MAX) len = PLUGIN_DRIVER_RX_MAX;
    os_mbuf_copydata(om, 0, len, data);

    rx_push(peer->conn_handle, RX_NOTIFY, attr_handle, len, data, len);
}

// --- Delivery (plugin task) ------------------------------------------------------------------

typedef struct {
    uint16_t conn_handle;
    mta_driver_instance_t* instance;
} find_conn_t;

static void find_conn_cb(mta_driver_instance_t* instance, void* arg) {
    find_conn_t* f = (find_conn_t*)arg;
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)mta_driver_instance_get_device(instance);
    if (ctx && ctx->peer && ctx->peer->conn_handle == f->conn_handle) f->instance = instance;
}

static void rx_deliver_all(void) {
    if (_rx_queue == NULL) return;

    for (int i = 0; i < RX_QUEUE_LEN && xQueueReceive(_rx_queue, _rx_item, 0) == pdTRUE; i++) {
        find_conn_t f = { .conn_handle = _rx_item->conn_handle, .instance = NULL };
        mta_driver_instance_foreach(find_conn_cb, &f);
        if (f.instance == NULL) continue; // disconnected meanwhile

        const char* event = NULL;
        switch (_rx_item->kind) {
        case RX_READ: event = "ble_read"; break;
        case RX_NOTIFY: event = "ble_notify"; break;
        case RX_SUBSCRIBED: event = "ble_subscribed"; break;
        case RX_SECURED: event = "ble_secured"; break;
        case RX_MTU: event = "ble_mtu"; break;
        default: continue;
        }

        _rx_current = _rx_item;
        _rx_current_ctx = (ble_conn_ctx_t*)mta_driver_instance_get_device(f.instance);
        mta_driver_instance_dispatch(f.instance, event, _rx_item->value, NULL);
        _rx_current = NULL;
        _rx_current_ctx = NULL;
    }
}

const uint8_t* plugin_driver_rx_data(ble_conn_ctx_t* ctx, int* len, uint16_t* attr_handle) {
    if (_rx_current == NULL || ctx == NULL || ctx != _rx_current_ctx) return NULL;
    if (_rx_current->kind != RX_READ && _rx_current->kind != RX_NOTIFY) return NULL;

    if (len) *len = _rx_current->len;
    if (attr_handle) *attr_handle = _rx_current->attr_handle;
    return _rx_current->data;
}
