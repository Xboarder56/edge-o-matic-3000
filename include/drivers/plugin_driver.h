#ifndef __drivers__plugin_driver_h
#define __drivers__plugin_driver_h

#ifdef __cplusplus
extern "C" {
#endif

#include "bluetooth_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "mt_actions.h"
#include <stdbool.h>

/**
 * @brief Maximum TX buffer size for plugin-driven BLE writes
 */
#define PLUGIN_DRIVER_TX_MAX 64
#define PLUGIN_DRIVER_TX_MAX_RETRIES 5

/**
 * @brief Maximum bytes kept from one GATT read or notification
 */
#define PLUGIN_DRIVER_RX_MAX 128

/**
 * @brief GATT procedures a plugin can request (one at a time per connection)
 */
typedef enum {
    BLE_OP_NONE = 0,
    BLE_OP_READ,
    BLE_OP_SUBSCRIBE, // descriptor discovery, then CCCD write
    BLE_OP_MTU,
} ble_op_t;

/**
 * @brief BLE-specific connection context
 *
 * Owns the GATT characteristic handles and the async TX pipe for a single
 * BLE connection. Attached to an mta_driver_instance_t via the I/O backend
 * context pointer. Freed when the I/O backend close callback fires.
 */
typedef struct ble_conn_ctx {
    peer_t* peer; // BLE peer handle (not owned)

    // TX characteristics discovered during GATT service discovery
    struct ble_gatt_chr tx_chr;
    bool has_tx_chr;
    struct ble_gatt_chr tx_no_rsp_chr;
    bool has_tx_no_rsp_chr;

    // Async TX pipe: plugin writes queue here, flushed in tick
    char pending_tx[PLUGIN_DRIVER_TX_MAX];
    int pending_len;
    bool pending_tx_flag;  // true = GATT write in flight
    bool use_write_no_rsp; // use write-no-response characteristic
    SemaphoreHandle_t tx_mutex;
    TickType_t retry_after; // Tick count before which flush retries are suppressed
    uint8_t tx_retry_count; // Current EBUSY retry count (reset on success)

    // Opt-in extensions (ble_use_characteristic, ble_read, ble_subscribe, ble_secure,
    // ble_request_mtu). All zero unless a plugin calls them, so existing drivers behave as before.
    uint16_t tx_select_handle; // write target chosen by ble_use_characteristic (0 = default)
    uint8_t tx_select_props;   // its GATT properties
    uint16_t pending_handle;   // target captured when the pending write was queued (0 = default)

    ble_op_t op_requested;   // waiting to start on the plugin task
    ble_op_t op_running;     // in flight in NimBLE
    uint16_t op_handle;      // characteristic value handle (read / subscribe)
    uint16_t op_end_handle;  // last handle to search for the CCCD (subscribe)
    uint16_t op_cccd_handle; // CCCD found by descriptor discovery
    uint16_t op_cccd_value;  // 0 off, 1 notify, 2 indicate
    uint8_t read_buf[PLUGIN_DRIVER_RX_MAX];
    uint16_t read_len;

    bool rx_enabled;       // route notifications/indications to the plugin
    bool secure_requested; // deliver ble_secured on the next encryption change
} ble_conn_ctx_t;

/**
 * @brief Initialize the plugin driver subsystem.
 *
 * Registers event handlers for driver-relevant events (EVT_SPEED_CHANGE, etc.)
 * so that connected driver instances receive events through their persistent
 * scope rather than the generic action_manager broadcast.
 */
void plugin_driver_init(void);

/**
 * @brief Attempt to claim a BLE peer for a loaded ble_driver plugin.
 *
 * Iterates loaded plugins looking for type="ble_driver" with a matching
 * bleName or bleNamePrefix. If found, performs GATT service discovery,
 * creates a ble_conn_ctx_t and an mta_driver_instance_t, and fires the
 * plugin's "connect" event.
 *
 * @param peer BLE peer to claim
 * @return true if a plugin claimed the peer, false if no match
 */
bool plugin_driver_try_claim(peer_t* peer);

/**
 * @brief Release a BLE connection managed by a plugin.
 *
 * Finds the driver instance associated with the peer, removes it from the
 * global list, and destroys it (fires "disconnect" event, frees scope and
 * BLE context).
 *
 * @param peer BLE peer to release
 */
void plugin_driver_release_peer(peer_t* peer);

/**
 * @brief Tick all active plugin connections.
 *
 * Flushes pending TX buffers for each connection. Called from the
 * plugin task loop.
 */
void plugin_driver_tick(void);

/**
 * @brief Enumerate connected plugin-driven BLE peers.
 *
 * Calls cb for each active connection's peer.
 *
 * @param cb Callback receiving each peer
 * @param arg Opaque argument passed through to cb
 */
typedef void (*plugin_driver_enum_cb_t)(peer_t* peer, void* arg);
void plugin_driver_enumerate(plugin_driver_enum_cb_t cb, void* arg);

/**
 * @brief Append one object per bound BLE device ({"name", "plugin"}) to a cJSON array.
 *
 * Safe to call from any task: reads a small table kept by try_claim / release_peer rather
 * than walking live driver instances.
 */
void plugin_driver_list_devices(cJSON* array);

/**
 * @brief Pre-filter: check if any loaded ble_driver plugin can handle a device name.
 *
 * Used at scan time to avoid showing unpairable devices in the list.
 *
 * @param name Advertised BLE device name
 * @return true if at least one plugin matches
 */
bool plugin_driver_can_handle_name(const char* name);

/**
 * @brief Queue a GATT procedure for a connection (started from the plugin task).
 * @return 0 if queued, -1 if another procedure is pending or running
 */
int plugin_driver_request_op(ble_conn_ctx_t* ctx, ble_op_t op, uint16_t handle, uint16_t cccd_value);

/**
 * @brief Start link encryption (bonding with LE Secure Connections, "Just Works").
 * The result arrives as the plugin's "ble_secured" event.
 * @return 0 if started or already encrypted, negative NimBLE error otherwise
 */
int plugin_driver_secure(ble_conn_ctx_t* ctx);

/**
 * @brief Find a discovered characteristic by UUID text ("150A", "0x150A", 8-hex 32-bit, or a
 * 36-character 128-bit UUID). Returns NULL if the peer has no such characteristic.
 */
const struct ble_gatt_chr* plugin_driver_find_chr(
    ble_conn_ctx_t* ctx, const char* uuid_text, uint16_t* end_handle
);

/**
 * @brief Received data being delivered to a plugin (inside ble_read / ble_notify), or NULL.
 *
 * Only valid on the plugin task during dispatch, and only for the connection that is ctx.
 */
const uint8_t* plugin_driver_rx_data(ble_conn_ctx_t* ctx, int* len, uint16_t* attr_handle);

/**
 * @brief UUID text of the characteristic whose data is being delivered ("" if none).
 * @param buf At least BLE_UUID_STR_LEN bytes
 */
const char* plugin_driver_rx_uuid(ble_conn_ctx_t* ctx, char* buf);

/**
 * @brief Called by the GAP handler (NimBLE host task) for notifications and indications.
 */
void plugin_driver_on_notify(peer_t* peer, uint16_t attr_handle, struct os_mbuf* om);

/**
 * @brief Called by the GAP handler (NimBLE host task) when link encryption changes.
 */
void plugin_driver_on_enc_change(peer_t* peer, int status);

#ifdef __cplusplus
}
#endif

#endif
