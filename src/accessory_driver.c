#include "accessory_driver.h"
#include "drivers/sc16is740.h"
#include "eom-hal.h"
#include "esp_log.h"
#include "maus_bus.h"
#include "system/event_manager.h"
#include "tscode.h"
#include <string.h>

static const char* TAG = "accessory_driver";
static uint8_t last_speed;

static maus_bus_err_t
_accessory_read(uint8_t address, uint8_t subaddress, uint8_t* data, size_t len) {
    maus_bus_err_t err = MAUS_BUS_OK;
    eom_hal_accessory_master_read_register(address, subaddress, data, len);
    return err;
}

static maus_bus_err_t
_accessory_write(uint8_t address, uint8_t subaddress, uint8_t* data, size_t len) {
    maus_bus_err_t err = MAUS_BUS_OK;
    eom_hal_accessory_master_write_register(address, subaddress, data, len);
    return err;
}

static maus_bus_err_t _accessory_probe(uint8_t address) {
    maus_bus_err_t err = MAUS_BUS_OK;
    if (EOM_HAL_OK != eom_hal_accessory_master_probe(address)) err = MAUS_BUS_FAIL;
    return err;
}

static void _device_speed_cb(
    maus_bus_driver_t* driver, maus_bus_device_t* device, maus_bus_address_t address, void* ptr
) {
    uint8_t speed = *((uint8_t*)ptr);

    if (driver->uart != NULL && driver->uart->transmit != NULL) {
        if (device->features.tscode) {
            tscode_command_t cmd = TSCODE_COMMAND_DEFAULT;
            cmd.type = speed > 0 ? TSCODE_RECIPROCATING_MOVE : TSCODE_CONDITIONAL_STOP;
            cmd.speed = (tscode_unit_t*)malloc(sizeof(tscode_unit_t));
            if (cmd.speed == NULL) {
                ESP_LOGE(TAG, "_device_speed_cb: out of memory");
                return;
            }
            cmd.speed->unit = TSCODE_UNIT_BYTE;
            cmd.speed->value = speed;
            ESP_LOGD(TAG, " -> TSCODE Device: %s", device->product_name);

            char buffer[120] = "";
            tscode_serialize_command(buffer, &cmd, 120);

            ESP_LOGD(TAG, "    -> Serialized: %s", buffer);
            tscode_dispose_command(&cmd);

            driver->uart->transmit((uint8_t*)buffer, strlen(buffer));
        }
    }
}

static void _evt_speed_change(
    const char* event,
    EVENT_HANDLER_ARG_TYPE event_arg_ptr,
    int speed,
    EVENT_HANDLER_ARG_TYPE handler_arg
) {
    if (speed == last_speed) return;
    last_speed = speed;

    ESP_LOGD(TAG, "_evt_speed_change(%d);", speed);

    maus_bus_enumerate_devices(_device_speed_cb, &last_speed);
}

static void _halt_device(
    maus_bus_driver_t* driver, maus_bus_device_t* device, maus_bus_address_t address, void* arg
) {
    if (!device->features.tscode) return;
    // S00 is immediate halt. The bundled serializer emits T00 for this one opcode.
    uint8_t halt[] = "S00\n";
    // The public HAL writes return void. Probe the actual UART transport after submission
    // to catch an unreachable adapter; there is no protocol-level stop acknowledgement.
    uint8_t transport_addr = device->features.serial ? SC16_ADDRESS : 0x69;
    if (!driver || !driver->uart || !driver->uart->transmit ||
        driver->uart->transmit(halt, sizeof(halt) - 1) != MAUS_BUS_OK ||
        eom_hal_accessory_master_probe(transport_addr) != EOM_HAL_OK) {
        ESP_LOGE(TAG, "Could not send halt to %s", device->product_name);
        *(bool*)arg = false;
    }
}

bool accessory_driver_halt(void) {
    bool ok = true;
    last_speed = 0;
    return maus_bus_enumerate_devices(_halt_device, &ok) == MAUS_BUS_OK && ok;
}

static void _evt_arousal_change(
    const char* event,
    EVENT_HANDLER_ARG_TYPE event_arg_ptr,
    int arousal,
    EVENT_HANDLER_ARG_TYPE handler_arg
) {
    static uint16_t last_value = 0;
    if (arousal == last_value) return;
    last_value = arousal;

    ESP_LOGD(TAG, "_evt_arousal_change(%d);", arousal);
}

void accessory_driver_init(void) {
    maus_bus_config_t config = {
        .read = _accessory_read,
        .write = _accessory_write,
        .probe = _accessory_probe,
    };

    maus_bus_init(&config);

    event_manager_register_handler(EVT_SPEED_CHANGE, _evt_speed_change, NULL);
    event_manager_register_handler(EVT_AROUSAL_CHANGE, _evt_arousal_change, NULL);
}