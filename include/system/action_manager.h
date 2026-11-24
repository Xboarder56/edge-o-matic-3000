#ifndef __system__action_manager_c
#define __system__action_manager_c

#ifdef __cplusplus
extern "C" {
#endif

#include "mt_actions.h"
#include <stddef.h>

void action_manager_init(void);
void action_manager_load_plugin(const char* filename);
void action_manager_load_all_plugins(void);
void action_manager_dispatch_event(const char* event, int val);

size_t action_manager_get_plugin_count(void);
mta_plugin_t* action_manager_get_plugin(size_t index);
mta_plugin_t* action_manager_find_plugin(const char* name);

/**
 * @brief Invoke one event on one loaded feature plugin, with that plugin's own scope.
 *
 * @return -2 if the plugin is not loaded (or is a ble_driver plugin, whose events go to driver
 * instances), otherwise the mt-actions result (-1 when the plugin has no handler for the event).
 */
int action_manager_invoke_plugin_event(mta_plugin_t* plugin, const char* event, int arg);

/**
 * @brief Save plugin config to disk
 *
 * Serializes plugin config and writes to /plugincfg/<name>.json
 *
 * @param plugin Plugin instance
 * @param user_data Opaque pointer that can be assigned during the callback registration.
 * @return true if saved successfully, false otherwise
 */
bool action_manager_save_plugin_config(mta_plugin_t* plugin, void* user_data);

#ifdef __cplusplus
}
#endif

#endif
