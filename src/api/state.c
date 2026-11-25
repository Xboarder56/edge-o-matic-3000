/**
 * Remote-control WebSocket commands added in 2.1.0-rc.5: device state snapshot, denial counter
 * reset, control lock, plugin listing and plugin remote events, and bound Bluetooth devices.
 */
#include "api/index.h"
#include "config.h"
#include "drivers/plugin_driver.h"
#include "eom-hal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "orgasm_control.h"
#include "orgasm_detection.h"
#include "system/action_manager.h"
#include "system/websocket_handler.h"
#include "ui/control_lock.h"
#include "version.h"
#include <ctype.h>
#include <string.h>

static const char* TAG = "api/state";

// Owned by the stats page, which counts denials while any page is open.
extern uint16_t edging_stats_page_get_denial_count(void);
extern void edging_stats_page_reset_denial_count(void);

// Plugins opt in to remote triggering by naming a handler with this prefix.
#define REMOTE_EVENT_PREFIX "remote_"
#define REMOTE_EVENT_NAME_MAX 24

/**
 * {"getState": {}}: one snapshot of everything a client needs to resync after connecting.
 */
static command_err_t cmd_get_state(cJSON* command, cJSON* response, websocket_client_t* client) {
    cJSON_AddStringToObject(response, "runMode", orgasm_control_get_output_mode_str());
    cJSON_AddNumberToObject(response, "mode", orgasm_control_get_output_mode());
    cJSON_AddNumberToObject(response, "motor", eom_hal_get_motor_speed());
    cJSON_AddNumberToObject(response, "motorMax", Config.motor_max_speed);
    cJSON_AddNumberToObject(response, "arousal", orgasm_control_get_arousal());
    cJSON_AddNumberToObject(response, "threshold", orgasm_control_get_arousal_threshold());
    cJSON_AddNumberToObject(response, "pressure", orgasm_control_get_last_pressure());
    cJSON_AddNumberToObject(response, "denials", edging_stats_page_get_denial_count());
    cJSON_AddBoolToObject(response, "pluginControl", orgasm_control_is_plugin_control());

    cJSON* lock = cJSON_AddObjectToObject(response, "lock");
    cJSON_AddBoolToObject(lock, "locked", control_lock_is_locked());
    cJSON_AddStringToObject(lock, "reason", control_lock_get_reason());

    cJSON* detect = cJSON_AddObjectToObject(response, "detection");
    cJSON_AddBoolToObject(detect, "armed", Config.od_detection_armed);
    cJSON_AddStringToObject(detect, "state", orgasm_detection_get_state_str());

    cJSON_AddBoolToObject(response, "wifiOn", Config.wifi_on);
    cJSON_AddBoolToObject(response, "btOn", Config.bt_on);
    cJSON_AddNumberToObject(
        response, "freeHeap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
    );
    cJSON_AddStringToObject(response, "fwVersion", EOM_VERSION);
    cJSON_AddNumberToObject(response, "millis", esp_timer_get_time() / 1000);
    return CMD_OK;
}

static const websocket_command_t cmd_get_state_s = {
    .command = "getState",
    .func = &cmd_get_state,
};

/**
 * {"resetDenials": {}}: same as "Reset Denial Count" on the device.
 */
static command_err_t
cmd_reset_denials(cJSON* command, cJSON* response, websocket_client_t* client) {
    edging_stats_page_reset_denial_count();
    cJSON_AddNumberToObject(response, "denials", edging_stats_page_get_denial_count());
    return CMD_OK;
}

static const websocket_command_t cmd_reset_denials_s = {
    .command = "resetDenials",
    .func = &cmd_reset_denials,
};

/**
 * {"setLock": {"locked": true, "reason": "Session running"}} or {"setLock": {"locked": false}}.
 * Locks the device buttons and encoder. Holding BACK+OK for 5 seconds always unlocks.
 */
static command_err_t cmd_set_lock(cJSON* command, cJSON* response, websocket_client_t* client) {
    if (!cJSON_IsObject(command)) return CMD_ARG_ERR;

    cJSON* locked = cJSON_GetObjectItem(command, "locked");
    if (!cJSON_IsBool(locked)) return CMD_ARG_ERR;

    if (cJSON_IsTrue(locked)) {
        cJSON* reason = cJSON_GetObjectItem(command, "reason");
        const char* reason_str =
            (cJSON_IsString(reason) && reason->valuestring) ? reason->valuestring : "Remote lock";
        control_lock_engage(reason_str);
    } else {
        control_lock_release();
    }

    cJSON_AddBoolToObject(response, "locked", control_lock_is_locked());
    cJSON_AddStringToObject(response, "reason", control_lock_get_reason());
    return CMD_OK;
}

static const websocket_command_t cmd_set_lock_s = {
    .command = "setLock",
    .func = &cmd_set_lock,
};

/**
 * {"pluginList": {}}: loaded plugins.
 */
static command_err_t cmd_plugin_list(cJSON* command, cJSON* response, websocket_client_t* client) {
    cJSON* plugins = cJSON_AddArrayToObject(response, "plugins");
    size_t count = action_manager_get_plugin_count();

    for (size_t i = 0; i < count; i++) {
        mta_plugin_t* plugin = action_manager_get_plugin(i);
        if (plugin == NULL) continue;

        const char* name = mta_plugin_get_name(plugin);
        const char* display = mta_plugin_get_display_name(plugin);
        const char* type = mta_plugin_get_type(plugin);
        const char* description = mta_plugin_get_description(plugin);

        cJSON* entry = cJSON_CreateObject();
        if (entry == NULL) return CMD_FAIL;
        cJSON_AddStringToObject(entry, "name", name ? name : "");
        cJSON_AddStringToObject(entry, "displayName", display ? display : (name ? name : ""));
        cJSON_AddStringToObject(entry, "type", type ? type : "");
        cJSON_AddStringToObject(entry, "description", description ? description : "");
        cJSON_AddItemToArray(plugins, entry);
    }

    return CMD_OK;
}

static const websocket_command_t cmd_plugin_list_s = {
    .command = "pluginList",
    .func = &cmd_plugin_list,
};

static bool _valid_event_suffix(const char* s) {
    size_t len = strlen(s);
    if (len == 0 || len > REMOTE_EVENT_NAME_MAX) return false;
    for (size_t i = 0; i < len; i++) {
        if (!(islower((unsigned char)s[i]) || isdigit((unsigned char)s[i]) || s[i] == '_')) {
            return false;
        }
    }
    return true;
}

/**
 * {"pluginEvent": {"plugin": "smart-lock", "event": "release", "value": 0}} runs the plugin's
 * "remote_release" handler. Only handlers named remote_* can be triggered, so a plugin decides
 * what is remotely callable. Feature plugins only (not ble_driver plugins).
 */
static command_err_t
cmd_plugin_event(cJSON* command, cJSON* response, websocket_client_t* client) {
    if (!cJSON_IsObject(command)) return CMD_ARG_ERR;

    cJSON* plugin_name = cJSON_GetObjectItem(command, "plugin");
    cJSON* event = cJSON_GetObjectItem(command, "event");
    cJSON* value = cJSON_GetObjectItem(command, "value");

    if (!cJSON_IsString(plugin_name) || !cJSON_IsString(event) || event->valuestring == NULL ||
        !_valid_event_suffix(event->valuestring)) {
        return CMD_ARG_ERR;
    }

    char handler[sizeof(REMOTE_EVENT_PREFIX) + REMOTE_EVENT_NAME_MAX];
    snprintf(handler, sizeof(handler), REMOTE_EVENT_PREFIX "%s", event->valuestring);

    cJSON_AddStringToObject(response, "plugin", plugin_name->valuestring);
    cJSON_AddStringToObject(response, "event", handler);

    mta_plugin_t* plugin = action_manager_find_plugin(plugin_name->valuestring);
    if (plugin == NULL) {
        cJSON_AddStringToObject(response, "error", "plugin not loaded");
        return CMD_ARG_ERR;
    }

    int arg = cJSON_IsNumber(value) ? value->valueint : 0;
    int rc = action_manager_invoke_plugin_event(plugin, handler, arg);
    ESP_LOGI(TAG, "pluginEvent %s.%s(%d) => %d", plugin_name->valuestring, handler, arg, rc);

    if (rc == -2) {
        cJSON_AddStringToObject(response, "error", "driver plugins cannot take remote events");
        return CMD_ARG_ERR;
    } else if (rc == -1) {
        cJSON_AddStringToObject(response, "error", "plugin has no handler for this event");
        return CMD_ARG_ERR;
    }

    cJSON_AddBoolToObject(response, "handled", true);
    cJSON_AddNumberToObject(response, "status", rc);
    return rc == 0 ? CMD_OK : CMD_FAIL;
}

static const websocket_command_t cmd_plugin_event_s = {
    .command = "pluginEvent",
    .func = &cmd_plugin_event,
};

/**
 * {"getDevices": {}}: Bluetooth devices currently bound to a driver plugin.
 */
static command_err_t cmd_get_devices(cJSON* command, cJSON* response, websocket_client_t* client) {
    cJSON* devices = cJSON_AddArrayToObject(response, "devices");
    plugin_driver_list_devices(devices);
    return CMD_OK;
}

static const websocket_command_t cmd_get_devices_s = {
    .command = "getDevices",
    .func = &cmd_get_devices,
};

void api_register_state(void) {
    websocket_register_command(&cmd_get_state_s);
    websocket_register_command(&cmd_reset_denials_s);
    websocket_register_command(&cmd_set_lock_s);
    websocket_register_command(&cmd_plugin_list_s);
    websocket_register_command(&cmd_plugin_event_s);
    websocket_register_command(&cmd_get_devices_s);
}
