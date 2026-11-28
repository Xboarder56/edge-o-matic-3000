#include "update_mode.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static portMUX_TYPE gate_lock = portMUX_INITIALIZER_UNLOCKED;
static bool updating;
static unsigned active_workers;

bool update_mode_try_work(void) {
    portENTER_CRITICAL(&gate_lock);
    bool admitted = !updating;
    if (admitted) ++active_workers;
    portEXIT_CRITICAL(&gate_lock);
    return admitted;
}

void update_mode_end_work(void) {
    portENTER_CRITICAL(&gate_lock);
    --active_workers;
    portEXIT_CRITICAL(&gate_lock);
}

bool update_mode_active(void) {
    portENTER_CRITICAL(&gate_lock);
    bool active = updating;
    portEXIT_CRITICAL(&gate_lock);
    return active;
}

void update_mode_leave(void) {
    portENTER_CRITICAL(&gate_lock);
    updating = false;
    portEXIT_CRITICAL(&gate_lock);
}

bool update_mode_enter(void) {
    TickType_t start = xTaskGetTickCount();

    portENTER_CRITICAL(&gate_lock);
    updating = true;
    portEXIT_CRITICAL(&gate_lock);

    for (;;) {
        portENTER_CRITICAL(&gate_lock);
        bool drained = active_workers == 0;
        portEXIT_CRITICAL(&gate_lock);
        if (drained) return true;
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(2000)) {
            update_mode_leave();
            return false;
        }
        vTaskDelay(1);
    }
}
