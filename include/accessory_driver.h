#ifndef __edge_o_matic__accessory_port_h
#define __edge_o_matic__accessory_port_h

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

void accessory_driver_init(void);
bool accessory_driver_halt(void);

#ifdef __cplusplus
}
#endif

#endif
