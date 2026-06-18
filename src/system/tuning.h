#pragma once

namespace kalshi {

bool pin_this_thread_to_core(unsigned core_id) noexcept;

bool set_realtime_priority(int prio) noexcept;

}