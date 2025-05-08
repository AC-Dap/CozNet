#ifndef TIME_H
#define TIME_H

#include <ctime>

#define BILLION 1000000000L
void add_ns(timespec *t, long ns) {
    time_t add_sec = ns / BILLION;
    ns = ns % BILLION;

    t->tv_nsec += ns;
    add_sec += t->tv_nsec / BILLION;
    t->tv_nsec = t->tv_nsec % BILLION;
    t->tv_sec += add_sec;
}

/**
    Returns true a <= b.
*/
inline bool time_passed(const timespec &a, const timespec &b) {
    return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec <= b.tv_nsec);
}

/**
    Returns a - b. Assumes a > b.
*/
timespec time_diff(const timespec &a, const timespec &b) {
    time_t sec_diff = a.tv_sec - b.tv_sec;
    long nsec_diff = 0;
    if (a.tv_nsec < b.tv_nsec) {
        sec_diff -= 1;
        nsec_diff = BILLION + a.tv_nsec - b.tv_nsec;
    } else {
        nsec_diff = a.tv_nsec - b.tv_nsec;
    }
    return { sec_diff, nsec_diff };
}

#endif //TIME_H
