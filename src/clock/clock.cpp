/* Enable POSIX.1-2008 API (setenv, tzset, etc.) which newlib hides under
 * _ANSI_SOURCE.  Must appear before any system header. */
#define _POSIX_C_SOURCE 200809L

#include "clock.hpp"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <zephyr/kernel.h>

namespace timekeeping {

void Clock::Init(uint64_t start_time_seconds) {
  setenv("TZ", "CET-1CEST", 1);
  tzset();

  struct timespec tspec;
  tspec.tv_sec = start_time_seconds;
  tspec.tv_nsec = 0;
  clock_settime(CLOCK_REALTIME, &tspec);
}

struct tm* Clock::GetTime() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return localtime(&tv.tv_sec);
}

time_t Clock::GetTimeUnix() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec;
}

}  // namespace timekeeping
