#include "clock.h"
#include <time.h>

#define TS_MIN 1704067200LL      // 2024-01-01
#define TS_MAX 4102444800LL      // 2100-01-01

static long long g_offset;
static int g_have;

static long long mono_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

void clock_learn(long long ts) {
  if (ts >= TS_MIN * 1000 && ts <= TS_MAX * 1000) ts /= 1000;
  if (ts < TS_MIN || ts > TS_MAX) return;
  g_offset = ts - mono_s();
  g_have = 1;
}

long long clock_now(void) {
  long long t = g_have ? g_offset + mono_s() : (long long)time(NULL);
  return t > 0 ? t : 1;
}
