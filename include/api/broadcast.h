#ifndef __broadcast_h
#define __broadcast_h

#ifdef __cplusplus
extern "C" {
#endif

void api_broadcast_config(void);
void api_broadcast_readings(void);
void api_broadcast_storage_status(void);
void api_broadcast_network_status(void);

/**
 * Forward device events (orgasm_denial, orgasm_permit, orgasm_start, mode_set) to clients that
 * subscribed with {"streamEvents": true}.
 */
void api_broadcast_events_init(void);

#ifdef __cplusplus
}
#endif

#endif
