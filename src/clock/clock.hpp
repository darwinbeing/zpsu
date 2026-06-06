#ifndef CLOCK_CLOCK_HPP_
#define CLOCK_CLOCK_HPP_

#include <stdint.h>
#include <time.h>

namespace timekeeping {

class Clock {
 public:
  static void Init(uint64_t start_time_seconds);
  static struct tm* GetTime();
  static time_t GetTimeUnix();
};

}  // namespace timekeeping

#endif  // CLOCK_CLOCK_HPP_
