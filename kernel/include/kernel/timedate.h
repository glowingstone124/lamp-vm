//
// Created by Max Wang on 2026/3/21.
//

#ifndef VM_TIMEDATE_H
#define VM_TIMEDATE_H
#include "platform.h"
typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
} DateTime;
static int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r > 0) != (b > 0))) {
        q --;
    }
    return q;
}

static int64_t floor_mod(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r != 0 && ((r > 0) != (b > 0))) {
        r += b;
    }
    return r;
}
void unix_to_datetime_utc(int64_t ts, DateTime *dt);
#endif //VM_TIMEDATE_H