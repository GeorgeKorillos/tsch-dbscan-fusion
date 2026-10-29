#include "pipeline_sample_timing.h"

#include <assert.h>
#include <stdio.h>
int main(void) {
    for (unsigned d = 0; d <= 640; d++) {
        clock_time_t r = pipeline_sample_delay((clock_time_t)d);
        assert(r >= 320 && r < 960);
        assert(r % 640 == d % 640);
        if (d >= 320) {
            assert(r == d);
        }
    }
    assert(pipeline_sample_delay(1) == 641);
    assert(pipeline_sample_delay(20) == 660);
    assert(pipeline_sample_delay(0) == 640);
    assert(pipeline_sample_delay(639) == 639);
    /* Timer target can wrap; unsigned difference still preserves the delay. */
    clock_time_t now = (clock_time_t)-20;
    clock_time_t next = (clock_time_t)(now + pipeline_sample_delay(1));
    assert((clock_time_t)(next - now) == 641);
    puts("PASS sample rescheduling: near-zero, full period, bounded phase corrections, "
         "clock wrap");
}
