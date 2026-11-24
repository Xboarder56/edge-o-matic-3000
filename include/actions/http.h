#ifndef __actions__http_h
#define __actions__http_h

#ifdef __cplusplus
extern "C" {
#endif

#include "mt_actions.h"
#include <stdbool.h>
#include <stdint.h>

// http_request() return values other than a request ID.
#define HTTP_REQ_BUSY -1
#define HTTP_REQ_INVALID -2
#define HTTP_REQ_NO_MEMORY -3

// Negative http_response payloads.
typedef enum {
    HTTP_ERR_CONNECTION = -1,
    HTTP_ERR_TIMEOUT = -2,
    HTTP_ERR_NO_MEMORY = -3,
} http_err_t;

/**
 * @brief Register the http_* host functions and the http_response event (permission net:http).
 */
void action_http_init(void);

/**
 * @brief Deliver a finished HTTP response, if there is one for this task.
 *
 * Call from the main loop with driver_task = false (feature plugins) and from the plugin task
 * with driver_task = true (BLE driver instances). Cheap when idle.
 */
void action_http_poll(bool driver_task);

/**
 * @brief Keep HTTPS from starting while something else needs the memory (a BLE connection being
 * set up). Calls nest; pair each hold(true) with hold(false). Plain HTTP is not affected.
 */
void action_http_hold_tls(bool hold);

/**
 * @brief Wait until no HTTPS request is running (its TLS buffers are freed).
 * @return true if idle, false if timeout_ms passed first
 */
bool action_http_wait_tls_idle(uint32_t timeout_ms);

// Longest an HTTPS request can run (its timeout plus a margin).
#define HTTP_REQUEST_MAX_MS 12000

#ifdef __cplusplus
}
#endif

#endif
