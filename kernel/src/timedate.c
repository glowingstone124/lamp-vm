//
// Created by Max Wang on 2026/3/21.
//

#include "../include/kernel/timedate.h"

static void civil_from_days(int64_t z, int *year, int *month, int *day) {
    z += 719468;

    const int64_t era = (z >=0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = (int64_t)yoe + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint32_t m = mp + (mp < 10 ? 3 : -9);

    *year = (int)(y + (m <= 2));
    *month = (int)m;
    *day = (int)d;
}

void unix_to_datetime_utc(int64_t ts, DateTime *dt) {
    const int64_t days = floor_div(ts, 86400);
    const int64_t rem = floor_mod(ts, 86400);
    dt->hour = (int)(rem/3600);
    dt->minute = (int)((rem % 3600) / 60);
    dt->second = (int)(rem % 60);

    civil_from_days(days, &dt->year, &dt->month, &dt->day);
}
