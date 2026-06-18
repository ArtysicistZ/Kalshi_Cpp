#include "system/tuning.h"

#include <pthread.h>
#include <sched.h>

namespace kalshi {

bool pin_this_thread_to_core(unsigned core_id) noexcept {

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    return rc == 0;

}

bool set_realtime_priority(int prio) noexcept {

    sched_param param;
    param.sched_priority = prio;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    return rc == 0;

}

}