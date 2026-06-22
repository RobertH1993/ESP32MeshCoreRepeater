#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include "driver/i2c_types.h"

void display_init(i2c_master_bus_handle_t i2c_bus);


#endif