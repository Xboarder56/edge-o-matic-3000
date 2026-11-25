#include "api/broadcast.h"
#include "cJSON.h"
#include "config_defs.h"
#include "eom-hal.h"
#include "orgasm_control.h"
#include "orgasm_detection.h"
#include "system/event_manager.h"
#include "system/websocket_handler.h"
#include <ctype.h>
#include <esp_timer.h>
#include <string.h>

// Denials counted since boot (or since "Reset Denial Count"), as shown on the device screen.
extern uint16_t edging_stats_page_get_denial_count(void);

void api_broadcast_config(void) {
    cJSON* payload = cJSON_CreateObject();
    cJSON* root = cJSON_AddObjectToObject(payload, "configList");

    config_to_json(root, &Config);

    websocket_broadcast(payload, WS_BROADCAST_SYSTEM);
    cJSON_Delete(payload);
}

void api_broadcast_readings(void) {
    cJSON* payload = cJSON_CreateObject();
    cJSON* root = cJSON_AddObjectToObject(payload, "readings");

    cJSON_AddNumberToObject(root, "pressure", orgasm_control_get_last_pressure());
    cJSON_AddNumberToObject(root, "pavg", orgasm_control_get_average_pressure());
    cJSON_AddNumberToObject(root, "motor", eom_hal_get_motor_speed());
    cJSON_AddNumberToObject(root, "arousal", orgasm_control_get_arousal());
    cJSON_AddNumberToObject(root, "denials", edging_stats_page_get_denial_count());
    cJSON_AddNumberToObject(root, "millis", esp_timer_get_time() / 1000);

    // Everything around this is deprecated and should be moved into its own broadcast.
    cJSON_AddStringToObject(root, "runMode", orgasm_control_get_output_mode_str());

    // Orgasm detection state
    cJSON_AddStringToObject(root, "detectState", orgasm_detection_get_state_str());
    cJSON_AddNumberToObject(root, "detectBaseline", orgasm_detection_get_baseline());
    cJSON_AddNumberToObject(root, "detectPeakCount", orgasm_detection_get_peak_count());
    cJSON_AddNumberToObject(root, "detectSustainedMs", orgasm_detection_get_sustained_ms());
    cJSON_AddNumberToObject(root, "detectLastIntervalMs", orgasm_detection_get_last_interval_ms());

    websocket_broadcast(payload, WS_BROADCAST_READINGS);
    cJSON_Delete(payload);
}

void api_broadcast_storage_status(void) {
    cJSON* payload = cJSON_CreateObject();
    cJSON* root = cJSON_AddObjectToObject(payload, "sdStatus");

    long long size = eom_hal_get_sd_size_bytes();
    cJSON_AddNumberToObject(root, "size", size);

    // Deprecated for now???
    cJSON_AddStringToObject(root, "type", "");

    websocket_broadcast(payload, WS_BROADCAST_SYSTEM);
    cJSON_Delete(payload);
}

void api_broadcast_network_status(void) {
    cJSON* payload = cJSON_CreateObject();
    cJSON* root = cJSON_AddObjectToObject(payload, "wifiStatus");

    // TODO: Actually get this from active connection
    cJSON_AddStringToObject(root, "ssid", Config.wifi_ssid);
    cJSON_AddStringToObject(root, "ip", "");
    cJSON_AddNumberToObject(root, "rssi", -1);

    websocket_broadcast(payload, WS_BROADCAST_SYSTEM);
    cJSON_Delete(payload);
}

/**
 * Event names on the wire match the plugin event names: "EVT_ORGASM_DENIAL" -> "orgasm_denial".
 */
static void _on_device_event(
    const char* event, EVENT_HANDLER_ARG_TYPE event_arg_ptr, int event_arg_int,
    EVENT_HANDLER_ARG_TYPE handler_arg
) {
    char name[32];
    size_t i = 0;
    const char* src = strncmp(event, "EVT_", 4) ? event : event + 4;

    for (; src[i] != '\0' && i < sizeof(name) - 1; i++) {
        name[i] = tolower((unsigned char)src[i]);
    }
    name[i] = '\0';

    cJSON* payload = cJSON_CreateObject();
    cJSON* root = cJSON_AddObjectToObject(payload, "event");
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddNumberToObject(root, "value", event_arg_int);
    cJSON_AddNumberToObject(root, "millis", esp_timer_get_time() / 1000);

    if (!strcmp(event, EVT_MODE_SET)) {
        cJSON_AddStringToObject(root, "runMode", orgasm_control_get_output_mode_str());
    } else if (!strcmp(event, EVT_LOCK_CHANGED)) {
        cJSON_AddBoolToObject(root, "locked", event_arg_int != 0);
        cJSON_AddStringToObject(root, "reason", event_arg_ptr ? (const char*)event_arg_ptr : "");
    }

    websocket_broadcast(payload, WS_BROADCAST_EVENTS);
    cJSON_Delete(payload);
}

void api_broadcast_events_init(void) {
    event_manager_register_handler(EVT_ORGASM_DENIAL, _on_device_event, NULL);
    event_manager_register_handler(EVT_ORGASM_PERMIT, _on_device_event, NULL);
    event_manager_register_handler(EVT_ORGASM_START, _on_device_event, NULL);
    event_manager_register_handler(EVT_EDGE_START, _on_device_event, NULL);
    event_manager_register_handler(EVT_LOCK_CHANGED, _on_device_event, NULL);
    event_manager_register_handler(EVT_CONFIG_CHANGED, _on_device_event, NULL);
    event_manager_register_handler(EVT_MODE_SET, _on_device_event, NULL);
}

/**
 * device_connect / device_disconnect go straight to WebSocket clients, not through the event
 * manager: they fire on the Bluetooth host task, where running plugin handlers could deadlock.
 */
void api_broadcast_device_event(const char* name, const char* plugin, bool connected) {
    cJSON* payload = cJSON_CreateObject();
    if (payload == NULL) return;
    cJSON* root = cJSON_AddObjectToObject(payload, "event");
    cJSON_AddStringToObject(root, "name", connected ? "device_connect" : "device_disconnect");
    cJSON_AddNumberToObject(root, "value", connected ? 1 : 0);
    cJSON_AddNumberToObject(root, "millis", esp_timer_get_time() / 1000);
    cJSON_AddStringToObject(root, "device", name ? name : "");
    cJSON_AddStringToObject(root, "plugin", plugin ? plugin : "");

    websocket_broadcast(payload, WS_BROADCAST_EVENTS);
    cJSON_Delete(payload);
}
