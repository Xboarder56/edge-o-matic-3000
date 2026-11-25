#ifndef __api_index_h
#define __api_index_h

#ifdef __cplusplus
extern "C" {
#endif

void api_register_fsutils(void);
void api_register_config(void);
void api_register_system(void);
void api_register_edging(void);
void api_register_state(void);

void api_broadcast_events_init(void);

static inline void api_register_all(void) {
    api_register_fsutils();
    api_register_config();
    api_register_system();
    api_register_edging();
    api_register_state();
    api_broadcast_events_init();
}

#ifdef __cplusplus
}
#endif

#endif
