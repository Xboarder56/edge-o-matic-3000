#include "api/index.h"
#include "eom-hal.h"
#include "system/websocket_handler.h"
#include "version.h"

static command_err_t
cmd_system_restart(cJSON* command, cJSON* response, websocket_client_t* client) {
    fflush(stdout);
    esp_restart();
    return CMD_NOT_FOUND;
}

static const websocket_command_t cmd_system_restart_s = {
    .command = "restart",
    .func = &cmd_system_restart,
};

static command_err_t cmd_system_time(cJSON* command, cJSON* response, websocket_client_t* client) {
    return CMD_OK;
}

static const websocket_command_t cmd_system_time_s = {
    .command = "time",
    .func = &cmd_system_time,
};

static command_err_t cmd_system_info(cJSON* command, cJSON* response, websocket_client_t* client) {
    char buf[64] = { 0 };
    eom_hal_get_device_serial(buf, 64);

    cJSON_AddStringToObject(response, "device", "Edge-o-Matic 3000");
    cJSON_AddStringToObject(response, "serial", buf);
    cJSON_AddStringToObject(response, "hwVersion", eom_hal_get_version());
    cJSON_AddStringToObject(response, "fwVersion", EOM_VERSION);
    return CMD_OK;
}

static const websocket_command_t cmd_system_info_s = {
    .command = "info",
    .func = &cmd_system_info,
};

/**
 * {"streamReadings": true}  subscribe to readings (and system status) broadcasts.
 * {"streamReadings": false} stop readings broadcasts (system status stays subscribed).
 * Any value other than false subscribes, as before.
 */
static command_err_t
cmd_system_stream_readings(cJSON* command, cJSON* response, websocket_client_t* client) {
    if (client == NULL) return CMD_NOT_FOUND;

    if (cJSON_IsFalse(command)) {
        client->broadcast_flags &= ~WS_BROADCAST_READINGS;
    } else {
        client->broadcast_flags |= WS_BROADCAST_READINGS;
        client->broadcast_flags |= WS_BROADCAST_SYSTEM;
    }

    return CMD_OK;
}

/**
 * {"streamEvents": true|false}: subscribe to or stop device event broadcasts, delivered as
 * {"event": {"name": "...", "value": n, "millis": n}}.
 */
static command_err_t
cmd_system_stream_events(cJSON* command, cJSON* response, websocket_client_t* client) {
    if (client == NULL) return CMD_NOT_FOUND;

    if (cJSON_IsFalse(command)) {
        client->broadcast_flags &= ~WS_BROADCAST_EVENTS;
    } else {
        client->broadcast_flags |= WS_BROADCAST_EVENTS;
    }

    return CMD_OK;
}

static const websocket_command_t cmd_system_stream_readings_s = {
    .command = "streamReadings",
    .func = &cmd_system_stream_readings,
};

static const websocket_command_t cmd_system_stream_events_s = {
    .command = "streamEvents",
    .func = &cmd_system_stream_events,
};

void api_register_system(void) {
    websocket_register_command(&cmd_system_restart_s);
    websocket_register_command(&cmd_system_time_s);
    websocket_register_command(&cmd_system_info_s);
    websocket_register_command(&cmd_system_stream_readings_s);
    websocket_register_command(&cmd_system_stream_events_s);
}