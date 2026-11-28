#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Only the main task enters/leaves update mode. Entry rejects new work while
// draining existing operations. On timeout, normal work is admitted again.
bool update_mode_enter(void);
void update_mode_leave(void);
bool update_mode_active(void);

// Background tasks must pair every successful admission with end_work.
bool update_mode_try_work(void);
void update_mode_end_work(void);

#ifdef __cplusplus
}
#endif
