// Wall clock without touching the system clock: an offset from CLOCK_MONOTONIC learned from client timestamps.
#ifndef GKSDCP_CLOCK_H
#define GKSDCP_CLOCK_H

// Accepts seconds in [1704067200, 4102444800] or the same range in milliseconds; anything else is ignored.
void clock_learn(long long ts);
// Seconds: the learned offset plus CLOCK_MONOTONIC, else time(NULL). Never 0.
long long clock_now(void);
#endif
