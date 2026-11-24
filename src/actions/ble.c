#include "actions/ble.h"
#include "drivers/plugin_driver.h"
#include "host/ble_hs.h"
#include <esp_log.h>
#include <string.h>

static const char* TAG = "actions:ble";

/**
 * Retrieve the BLE connection context from the current scope.
 *
 * The scope's user_data is set to the ble_conn_ctx_t by plugin_driver_try_claim.
 * Returns NULL if the scope has no BLE context or the peer is not set.
 */
static ble_conn_ctx_t* get_ble_ctx(mta_scope_t* scope) {
    ble_conn_ctx_t* ctx = (ble_conn_ctx_t*)mta_scope_get_user_data(scope);
    if (!ctx || !ctx->peer) return NULL;
    return ctx;
}

/**
 * Copy the validated data argument into the BLE TX buffer.
 *
 * Caller is responsible for arg validation via mta_read_args before calling.
 * Accepts either a string or a byte-array argument at args[0].
 *
 * @param plugin      Plugin context
 * @param scope       Execution scope (carries BLE connection via user_data)
 * @param args        Validated argument array; args[0] = data
 * @param no_response true = write-without-response characteristic
 * @param fn_name     Function name used in error messages
 * @return 0 on success, -1 on mta_raise error
 */
static int queue_ble_write(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, bool no_response, const char* fn_name
) {
    ble_conn_ctx_t* ctx = get_ble_ctx(scope);
    if (!ctx) {
        return mta_raise(
            plugin, MTA_RT_ERR_HOST_DISPATCH_FAILED, "%s: no BLE device context", fn_name
        );
    }

    if (ctx->tx_select_handle == 0 &&
        (no_response ? (!ctx->has_tx_no_rsp_chr && !ctx->has_tx_chr) : !ctx->has_tx_chr)) {
        return mta_raise(
            plugin, MTA_RT_ERR_HOST_DISPATCH_FAILED, "%s: no writable characteristic", fn_name
        );
    }

    uint8_t buf[PLUGIN_DRIVER_TX_MAX];
    int len = mta_arg_copy_bytes(plugin, scope, args, 0, buf, PLUGIN_DRIVER_TX_MAX - 1);

    if (len < 0) {
        return mta_raise(
            plugin, MTA_RT_ERR_TYPE_MISMATCH, "%s: data must be a string or byte array", fn_name
        );
    }

    if (xSemaphoreTake(ctx->tx_mutex, 1000UL / portTICK_RATE_MS)) {
        memcpy(ctx->pending_tx, buf, len);
        ctx->pending_tx[len] = '\0';
        ctx->pending_len = len;
        ctx->use_write_no_rsp = no_response;
        ctx->pending_handle = ctx->tx_select_handle; // 0 unless ble_use_characteristic was called

        xSemaphoreGive(ctx->tx_mutex);
        ESP_LOGD(TAG, "%s: queued %d bytes", fn_name, len);
    } else {
        return mta_raise(
            plugin, MTA_RT_ERR_HOST_DISPATCH_FAILED, "%s: timeout waiting for TX mutex", fn_name
        );
    }

    return 0;
}

/**
 * Write data to a BLE GATT characteristic (with response).
 *
 * @plugin ble_write
 * @module ble
 * @arg data:any Data to send (string or byte array)
 * @returns void
 */
static int
host_ble_write(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = {
        { "data", MTA_KIND_ANY },
    };

    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "ble_write");
    if (ret != 0) return ret;

    int rc = queue_ble_write(plugin, scope, args, false, "ble_write");
    if (rc != 0) return rc;

    return mta_return_void(plugin, scope);
}

/**
 * Write data to a BLE GATT characteristic (without response).
 *
 * @plugin ble_write_no_response
 * @module ble
 * @arg data:any Data to send (string or byte array)
 * @returns void
 */
static int host_ble_write_no_response(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    static const mta_arg_spec_t specs[] = {
        { "data", MTA_KIND_ANY },
    };

    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "ble_write_no_response");
    if (ret != 0) return ret;

    int rc = queue_ble_write(plugin, scope, args, true, "ble_write_no_response");
    if (rc != 0) return rc;

    return mta_return_void(plugin, scope);
}

// ---------------------------------------------------------------------------------------------
// Opt-in extensions. None of these change anything for a plugin that does not call them.
// ---------------------------------------------------------------------------------------------

static int need_ctx(mta_plugin_t* plugin, mta_scope_t* scope, const char* fn, ble_conn_ctx_t** out) {
    *out = get_ble_ctx(scope);
    if (*out == NULL) {
        return mta_raise(plugin, MTA_RT_ERR_HOST_DISPATCH_FAILED, "%s: no BLE device context", fn);
    }
    return 0;
}

static int read_one_string(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count, const char* name,
    const char* fn, const char** out
) {
    const mta_arg_spec_t specs[] = { { name, MTA_KIND_STRING } };
    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, fn);
    if (ret == 0) *out = v[0].val.s;
    return ret;
}

/**
 * Send this connection's ble_write / ble_write_no_response to a specific characteristic instead
 * of the first writable one found. Applies to writes queued after this call.
 *
 * @plugin ble_use_characteristic
 * @module ble
 * @arg uuid:string "150A", "0x150A", 8 hex digits (32-bit) or a 36-character 128-bit UUID
 * @returns int 1 if found and writable, 0 otherwise (the previous target stays)
 */
static int host_ble_use_characteristic(
    mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count
) {
    const char* uuid = NULL;
    int ret = read_one_string(plugin, scope, args, arg_count, "uuid", "ble_use_characteristic", &uuid);
    if (ret != 0) return ret;

    ble_conn_ctx_t* ctx;
    ret = need_ctx(plugin, scope, "ble_use_characteristic", &ctx);
    if (ret != 0) return ret;

    const struct ble_gatt_chr* chr = plugin_driver_find_chr(ctx, uuid, NULL);
    uint8_t props = chr ? chr->properties : 0;

    if (!(props & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP))) {
        ESP_LOGW(TAG, "ble_use_characteristic: %s not found or not writable", uuid);
        return mta_return_int(plugin, scope, 0);
    }

    ctx->tx_select_handle = chr->val_handle;
    ctx->tx_select_props = props;
    return mta_return_int(plugin, scope, 1);
}

/**
 * Read a characteristic. The value arrives as the ble_read event; get it with ble_rx_* inside the
 * handler. One read or subscribe at a time per connection.
 *
 * @plugin ble_read
 * @module ble
 * @arg uuid:string Characteristic UUID
 * @returns int 0 if started, -1 if another read/subscribe is pending, -2 if not found, -3 if not
 * readable
 */
static int
host_ble_read(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    const char* uuid = NULL;
    int ret = read_one_string(plugin, scope, args, arg_count, "uuid", "ble_read", &uuid);
    if (ret != 0) return ret;

    ble_conn_ctx_t* ctx;
    ret = need_ctx(plugin, scope, "ble_read", &ctx);
    if (ret != 0) return ret;

    const struct ble_gatt_chr* chr = plugin_driver_find_chr(ctx, uuid, NULL);
    if (chr == NULL) return mta_return_int(plugin, scope, -2);
    if (!(chr->properties & BLE_GATT_CHR_PROP_READ)) return mta_return_int(plugin, scope, -3);

    return mta_return_int(
        plugin, scope, plugin_driver_request_op(ctx, BLE_OP_READ, chr->val_handle, 0)
    );
}

/**
 * Subscribe to (or stop) notifications or indications from a characteristic. The result arrives
 * as the ble_subscribed event; data then arrives as ble_notify events.
 *
 * @plugin ble_subscribe
 * @module ble
 * @arg uuid:string Characteristic UUID
 * @arg mode:int? 1 notifications (default), 2 indications, 0 stop
 * @returns int 0 if started, -1 if another read/subscribe is pending, -2 if not found, -3 if the
 * characteristic does not support it
 */
static int
host_ble_subscribe(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "uuid", MTA_KIND_STRING }, { "mode", MTA_KIND_INT } };

    if (arg_count < 1 || arg_count > 2) {
        return mta_raise(
            plugin, MTA_RT_ERR_ARG_COUNT_MISMATCH, "ble_subscribe: expected 1-2 args, got %d",
            arg_count
        );
    }

    mta_argv_t v[2];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, arg_count, v, "ble_subscribe");
    if (ret != 0) return ret;

    ble_conn_ctx_t* ctx;
    ret = need_ctx(plugin, scope, "ble_subscribe", &ctx);
    if (ret != 0) return ret;

    int mode = arg_count >= 2 ? v[1].val.i : 1;
    if (mode < 0 || mode > 2) return mta_return_int(plugin, scope, -3);

    uint16_t end_handle = 0;
    const struct ble_gatt_chr* chr = plugin_driver_find_chr(ctx, v[0].val.s, &end_handle);
    if (chr == NULL) return mta_return_int(plugin, scope, -2);

    if ((mode == 1 && !(chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) ||
        (mode == 2 && !(chr->properties & BLE_GATT_CHR_PROP_INDICATE))) {
        return mta_return_int(plugin, scope, -3);
    }

    ret = plugin_driver_request_op(ctx, BLE_OP_SUBSCRIBE, chr->val_handle, (uint16_t)mode);
    if (ret == 0) ctx->op_end_handle = end_handle;
    return mta_return_int(plugin, scope, ret);
}

/**
 * Encrypt the link, bonding with the device (LE Secure Connections, "Just Works"). Bonds are kept
 * across restarts. The result arrives as the ble_secured event.
 *
 * @plugin ble_secure
 * @module ble
 * @returns int 0 if started (or already encrypted), negative error otherwise
 */
static int
host_ble_secure(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "ble_secure");
    if (ret != 0) return ret;

    ble_conn_ctx_t* ctx;
    ret = need_ctx(plugin, scope, "ble_secure", &ctx);
    if (ret != 0) return ret;

    return mta_return_int(plugin, scope, plugin_driver_secure(ctx));
}

/**
 * Whether the link is currently encrypted.
 *
 * @plugin ble_is_secure
 * @module ble
 * @returns int 1 if encrypted, 0 if not
 */
static int
host_ble_is_secure(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "ble_is_secure");
    if (ret != 0) return ret;

    ble_conn_ctx_t* ctx;
    ret = need_ctx(plugin, scope, "ble_is_secure", &ctx);
    if (ret != 0) return ret;

    struct ble_gap_conn_desc desc;
    int secure = ble_gap_conn_find(ctx->peer->conn_handle, &desc) == 0 && desc.sec_state.encrypted;
    return mta_return_int(plugin, scope, secure);
}

/**
 * Negotiate a larger ATT MTU (the EOM offers 256). The result arrives as the ble_mtu event.
 *
 * @plugin ble_request_mtu
 * @module ble
 * @returns int 0 if started, -1 if another procedure is pending
 */
static int
host_ble_request_mtu(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "ble_request_mtu");
    if (ret != 0) return ret;

    ble_conn_ctx_t* ctx;
    ret = need_ctx(plugin, scope, "ble_request_mtu", &ctx);
    if (ret != 0) return ret;

    return mta_return_int(plugin, scope, plugin_driver_request_op(ctx, BLE_OP_MTU, 0, 0));
}

/**
 * Current ATT MTU of the connection (23 until a larger one is negotiated). The largest single
 * write is this minus 3.
 *
 * @plugin ble_mtu
 * @module ble
 * @returns int MTU in bytes
 */
static int
host_ble_mtu(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "ble_mtu");
    if (ret != 0) return ret;

    ble_conn_ctx_t* ctx;
    ret = need_ctx(plugin, scope, "ble_mtu", &ctx);
    if (ret != 0) return ret;

    return mta_return_int(plugin, scope, ble_att_mtu(ctx->peer->conn_handle));
}

/**
 * Whether a write is still queued or waiting for its response. Each new write replaces one that
 * has not been sent yet, so wait for 0 before sending the next frame of a sequence.
 *
 * @plugin ble_tx_busy
 * @module ble
 * @returns int 1 if busy, 0 if idle
 */
static int
host_ble_tx_busy(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "ble_tx_busy");
    if (ret != 0) return ret;

    ble_conn_ctx_t* ctx;
    ret = need_ctx(plugin, scope, "ble_tx_busy", &ctx);
    if (ret != 0) return ret;

    return mta_return_int(plugin, scope, ctx->pending_len > 0 || ctx->pending_tx_flag);
}

static const uint8_t* rx_data(mta_scope_t* scope, int* len) {
    ble_conn_ctx_t* ctx = get_ble_ctx(scope);
    const uint8_t* data = plugin_driver_rx_data(ctx, len, NULL);
    if (data == NULL) *len = 0;
    return data;
}

/**
 * Length of the data being delivered (inside ble_read or ble_notify; at most 128 bytes).
 *
 * @plugin ble_rx_length
 * @module ble
 * @returns int Bytes, 0 outside those events
 */
static int
host_ble_rx_length(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "ble_rx_length");
    if (ret != 0) return ret;

    int len;
    rx_data(scope, &len);
    return mta_return_int(plugin, scope, len);
}

/**
 * One byte of the data being delivered (inside ble_read or ble_notify).
 *
 * @plugin ble_rx_byte
 * @module ble
 * @arg index:int Byte offset
 * @returns int 0-255, or -1 if out of range
 */
static int
host_ble_rx_byte(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "index", MTA_KIND_INT } };
    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "ble_rx_byte");
    if (ret != 0) return ret;

    int len;
    const uint8_t* data = rx_data(scope, &len);
    int i = v[0].val.i;
    return mta_return_int(plugin, scope, (data && i >= 0 && i < len) ? data[i] : -1);
}

/**
 * Copy the data being delivered into a byte array (inside ble_read or ble_notify).
 *
 * @plugin ble_rx_copy
 * @module ble
 * @arg out:array Destination array; copies up to its capacity
 * @returns int Bytes copied
 */
static int
host_ble_rx_copy(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    static const mta_arg_spec_t specs[] = { { "out", MTA_KIND_ARRAY } };
    mta_argv_t v[1];
    int ret = mta_read_args(plugin, scope, args, arg_count, specs, 1, v, "ble_rx_copy");
    if (ret != 0) return ret;

    int len;
    const uint8_t* data = rx_data(scope, &len);
    mta_array_t* out = v[0].val.arr;
    int n = len < out->capacity ? len : out->capacity;

    for (int i = 0; i < n; i++) out->data[i] = data[i];
    out->length = n;
    return mta_return_int(plugin, scope, n);
}

/**
 * The data being delivered as text (inside ble_read or ble_notify; stops at a zero byte).
 *
 * @plugin ble_rx_string
 * @module ble
 * @returns string Text, "" outside those events
 */
static int
host_ble_rx_string(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "ble_rx_string");
    if (ret != 0) return ret;

    int len;
    const uint8_t* data = rx_data(scope, &len);
    char text[PLUGIN_DRIVER_RX_MAX + 1];
    if (data) memcpy(text, data, len);
    text[len] = '\0';
    return mta_return_string(plugin, scope, text);
}

/**
 * UUID of the characteristic the delivered data came from (inside ble_read or ble_notify).
 * 16-bit UUIDs are returned as "0x150b".
 *
 * @plugin ble_rx_uuid
 * @module ble
 * @returns string UUID text, "" outside those events
 */
static int
host_ble_rx_uuid(mta_plugin_t* plugin, mta_scope_t* scope, mta_arg_t* args, uint8_t arg_count) {
    int ret = mta_read_args(plugin, scope, args, arg_count, NULL, 0, NULL, "ble_rx_uuid");
    if (ret != 0) return ret;

    char buf[BLE_UUID_STR_LEN];
    return mta_return_string(plugin, scope, plugin_driver_rx_uuid(get_ble_ctx(scope), buf));
}

void action_ble_init(void) {
    mta_register_system_function("ble_write", host_ble_write, "ble:write");
    mta_register_system_function("ble_write_no_response", host_ble_write_no_response, "ble:write");

    mta_register_system_function("ble_use_characteristic", host_ble_use_characteristic, "ble:write");
    mta_register_system_function("ble_tx_busy", host_ble_tx_busy, "ble:write");
    mta_register_system_function("ble_request_mtu", host_ble_request_mtu, "ble:write");
    mta_register_system_function("ble_mtu", host_ble_mtu, "ble:write");
    mta_register_system_function("ble_read", host_ble_read, "ble:read");
    mta_register_system_function("ble_subscribe", host_ble_subscribe, "ble:notify");
    mta_register_system_function("ble_secure", host_ble_secure, "ble:secure");
    mta_register_system_function("ble_is_secure", host_ble_is_secure, "ble:secure");

    // Received data: only ever returns this plugin's own connection's data.
    mta_register_system_function("ble_rx_length", host_ble_rx_length, NULL);
    mta_register_system_function("ble_rx_byte", host_ble_rx_byte, NULL);
    mta_register_system_function("ble_rx_copy", host_ble_rx_copy, NULL);
    mta_register_system_function("ble_rx_string", host_ble_rx_string, NULL);
    mta_register_system_function("ble_rx_uuid", host_ble_rx_uuid, NULL);

    /**
     * Fires when a ble_read finishes. Get the value with ble_rx_* inside the handler.
     * @event ble_read
     * @module ble
     * @payload length:int Bytes read (at most 128), or a negative error
     */
    mta_register_event("ble_read", "ble:read");

    /**
     * Fires for each notification or indication from a subscribed characteristic. Get the data
     * with ble_rx_* (ble_rx_uuid tells which characteristic) inside the handler.
     * @event ble_notify
     * @module ble
     * @payload length:int Bytes received (at most 128)
     */
    mta_register_event("ble_notify", "ble:notify");

    /**
     * Fires when a ble_subscribe finishes.
     * @event ble_subscribed
     * @module ble
     * @payload status:int 0 on success, negative error otherwise
     */
    mta_register_event("ble_subscribed", "ble:notify");

    /**
     * Fires when a ble_secure finishes.
     * @event ble_secured
     * @module ble
     * @payload status:int 0 when the link is encrypted, negative error otherwise
     */
    mta_register_event("ble_secured", "ble:secure");

    /**
     * Fires when a ble_request_mtu finishes.
     * @event ble_mtu
     * @module ble
     * @payload mtu:int Negotiated ATT MTU, or a negative error
     */
    mta_register_event("ble_mtu", "ble:write");
}
