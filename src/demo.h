#include <stdint.h>

struct SensorReading {
    uint32_t seq;      // reading number (1, 2, 3...)
    int32_t  temp;     // temperature in units of 0.01C
};

