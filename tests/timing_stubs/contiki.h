#include <stdint.h>
#if TEST_CLOCK_WIDTH == 16
typedef uint16_t clock_time_t;
#else
typedef uint32_t clock_time_t;
#endif
#define CLOCK_SECOND 128UL
