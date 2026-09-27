#include "cfw/core/Clock.h"

#include <cstdio>

namespace cfw {

String formatIsoUtc(std::int64_t unixSeconds) {
    std::int64_t days = unixSeconds / 86400;
    std::int64_t secs = unixSeconds % 86400;
    if (secs < 0) {
        secs += 86400;
        --days;
    }
    // Days since 1970-01-01 to a proleptic Gregorian date, by 400-year eras
    // starting on 1 March (so the leap day ends the year).
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t dayOfEra = z - era * 146097;
    const std::int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const std::int64_t mp = (5 * dayOfYear + 2) / 153;
    const std::int64_t day = dayOfYear - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
    char text[128];
    std::snprintf(text, sizeof text, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", static_cast<long long>(year),
                  static_cast<long long>(month), static_cast<long long>(day), static_cast<long long>(secs / 3600),
                  static_cast<long long>(secs / 60 % 60), static_cast<long long>(secs % 60));
    return text;
}

} // namespace cfw
