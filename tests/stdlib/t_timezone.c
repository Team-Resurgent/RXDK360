/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/* Time zones: POSIX.1-2024 tm_gmtoff/tm_zone, TZ rules through localtime/mktime,
   the console-settings -> TZ rule converter, and the default TZ the runtime
   derives from the console's time zone setting. Shared by both consoles. */
/* setenv/unsetenv/strdup are POSIX. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "rxdk_test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xbox_tz.h>
#include <time.h>

/* The console's own time zone API, declared here so the test needs no SDK header.
   Same layout on both consoles (LONG is 32 bits, WCHAR 16). */
typedef struct {
    unsigned short wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} tz_systemtime;
typedef struct {
    long           Bias;
    unsigned short StandardName[32];
    tz_systemtime  StandardDate;
    long           StandardBias;
    unsigned short DaylightName[32];
    tz_systemtime  DaylightDate;
    long           DaylightBias;
} tz_information;
#if defined(__i386__)
#define TZ_API __attribute__((__stdcall__))
#else
#define TZ_API
#endif
unsigned long TZ_API GetTimeZoneInformation(tz_information *info);
#define TIME_ZONE_ID_STANDARD 1
#define TIME_ZONE_ID_DAYLIGHT 2

static void use_tz(const char *rule)
{
    setenv("TZ", rule, 1);
    tzset();
}

static void check_local(time_t t, int hour, int isdst, long gmtoff, const char *zone,
                        const char *desc)
{
    struct tm tm;
    char      msg[160];

    memset(&tm, 0x5a, sizeof tm);
    CHECK(localtime_r(&t, &tm) != NULL, desc);
    snprintf(msg, sizeof msg, "%s: hour", desc);
    CHECK_EQI(tm.tm_hour, hour, msg);
    snprintf(msg, sizeof msg, "%s: tm_isdst", desc);
    CHECK_EQI(tm.tm_isdst, isdst, msg);
    snprintf(msg, sizeof msg, "%s: tm_gmtoff", desc);
    CHECK_EQI(tm.tm_gmtoff, gmtoff, msg);
    snprintf(msg, sizeof msg, "%s: tm_zone", desc);
    CHECK_STR(tm.tm_zone ? tm.tm_zone : "(null)", zone, msg);
}

static void check_rule(const struct __xbox_tz_info *info, const char *want, const char *desc)
{
    char buf[__XBOX_TZ_RULE_MAX];
    int  n = __xbox_tz_rule(info, buf, sizeof buf);

    CHECK_EQI(n, (long)strlen(want), desc);
    CHECK_STR(n >= 0 ? buf : "(failed)", want, desc);
}

static struct __xbox_tz_info zone(long bias, long std_bias, long dst_bias, const char *std_name,
                                  const char *dst_name)
{
    struct __xbox_tz_info info;

    memset(&info, 0, sizeof info);
    info.bias = bias;
    info.std_bias = std_bias;
    info.dst_bias = dst_bias;
    strncpy(info.std_name, std_name, sizeof info.std_name - 1);
    strncpy(info.dst_name, dst_name, sizeof info.dst_name - 1);
    return info;
}

static void section_console_default(void)
{
    /* The runtime installs the console's time zone as TZ before main(). */
    const char *tz = getenv("TZ");
    DbgPrint("[T] INFO console TZ=%s\n", tz ? tz : "(unset)");
    CHECK(tz != NULL && *tz != '\0', "runtime sets a default TZ from the console settings");

    /* It must agree with the console's own time zone API. (On xenia every time
       zone setting reads as 0, so this only exercises UTC there.) */
    tz_information info;
    memset(&info, 0, sizeof info);
    unsigned long id = GetTimeZoneInformation(&info);
    long bias = info.Bias;
    if (id == TIME_ZONE_ID_DAYLIGHT)
        bias += info.DaylightBias;
    else if (id == TIME_ZONE_ID_STANDARD)
        bias += info.StandardBias;

    time_t    now = time(NULL);
    struct tm tm;
    CHECK(localtime_r(&now, &tm) != NULL, "localtime_r(now)");
    CHECK_EQI(tm.tm_gmtoff, -bias * 60, "localtime offset matches GetTimeZoneInformation");
    CHECK(tm.tm_zone != NULL && strlen(tm.tm_zone) >= 3, "localtime has a zone name");
}

static void section_utc(void)
{
    struct tm tm;
    time_t    t = 1719835200; /* 2024-07-01 12:00:00 UTC */

    memset(&tm, 0x5a, sizeof tm);
    CHECK(gmtime_r(&t, &tm) != NULL, "gmtime_r");
    CHECK_EQI(tm.tm_hour, 12, "gmtime_r hour");
    CHECK_EQI(tm.tm_gmtoff, 0, "gmtime_r tm_gmtoff is 0");
    CHECK_STR(tm.tm_zone ? tm.tm_zone : "(null)", "GMT", "gmtime_r tm_zone");

    memset(&tm, 0, sizeof tm);
    tm.tm_year = 124;
    tm.tm_mon = 6;
    tm.tm_mday = 1;
    tm.tm_hour = 12;
    tm.tm_gmtoff = 12345;
    CHECK_EQI(timegm(&tm), 1719835200, "timegm");
    CHECK_EQI(tm.tm_gmtoff, 0, "timegm sets tm_gmtoff to 0");
}

static void section_rules(void)
{
    use_tz("PST8PDT,M3.2.0/2,M11.1.0/2");
    check_local(1705320000, 4, 0, -8 * 3600, "PST", "US Pacific, winter");
    check_local(1719835200, 5, 1, -7 * 3600, "PDT", "US Pacific, summer");
    check_local(1710064799, 1, 0, -8 * 3600, "PST", "US Pacific, 1s before DST");
    check_local(1710064800, 3, 1, -7 * 3600, "PDT", "US Pacific, DST start");

    /* Southern hemisphere: daylight time spans the new year. */
    use_tz("AEST-10AEDT,M10.1.0,M4.1.0/3");
    check_local(1705320000, 23, 1, 11 * 3600, "AEDT", "Sydney, January");
    check_local(1719835200, 22, 0, 10 * 3600, "AEST", "Sydney, July");
    check_local(1712419199, 2, 1, 11 * 3600, "AEDT", "Sydney, 1s before DST ends");
    check_local(1712419200, 2, 0, 10 * 3600, "AEST", "Sydney, DST end");

    use_tz("<+0530>-5:30");
    check_local(1719835200, 17, 0, 19800, "+0530", "India, half-hour offset");

    /* mktime normalizes tm_gmtoff/tm_zone the same way localtime does. */
    use_tz("PST8PDT,M3.2.0/2,M11.1.0/2");
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 124;
    tm.tm_mon = 6;
    tm.tm_mday = 1;
    tm.tm_hour = 5;
    tm.tm_isdst = -1;
    CHECK_EQI(mktime(&tm), 1719835200, "mktime, summer local time");
    CHECK_EQI(tm.tm_isdst, 1, "mktime tm_isdst");
    CHECK_EQI(tm.tm_gmtoff, -7 * 3600, "mktime tm_gmtoff");
    CHECK_STR(tm.tm_zone ? tm.tm_zone : "(null)", "PDT", "mktime tm_zone");
}

static void section_rule_builder(void)
{
    struct __xbox_tz_info info;
    char                  small[8];

    /* US Pacific as the dashboard stores it. */
    info = zone(480, 0, -60, "PST", "PDT");
    info.dst_date = (struct __xbox_tz_date){ 3, 2, 0, 2, 0 };
    info.std_date = (struct __xbox_tz_date){ 11, 1, 0, 2, 0 };
    check_rule(&info, "<PST>8<PDT>7,M3.2.0/2,M11.1.0/2", "rule: US Pacific");

    /* Southern hemisphere, and a non-zero standard bias. */
    info = zone(-600, 0, -60, "AEST", "AEDT");
    info.dst_date = (struct __xbox_tz_date){ 10, 1, 0, 2, 0 };
    info.std_date = (struct __xbox_tz_date){ 4, 1, 0, 3, 0 };
    check_rule(&info, "<AEST>-10<AEDT>-11,M10.1.0/2,M4.1.0/3", "rule: Sydney");

    /* No transition dates: a fixed offset, and the standard bias is not applied
       (matching GetTimeZoneInformation when DST is off). */
    info = zone(-330, 15, -60, "IST", "IST");
    check_rule(&info, "<IST>-5:30", "rule: no DST, half-hour offset");

    /* Names tzset() cannot parse become numeric abbreviations. */
    info = zone(-330, 0, 0, "", "");
    check_rule(&info, "<+0530>-5:30", "rule: empty name");
    info = zone(60, 0, 0, "GMT 1", "");
    check_rule(&info, "<-0100>1", "rule: name with a space");
    info = zone(0, 0, 0, "UT", "");
    check_rule(&info, "<+0000>0", "rule: two-letter name");

    /* Minutes in the transition time, and the last-week marker. */
    info = zone(0, 0, -60, "GMT", "BST");
    info.dst_date = (struct __xbox_tz_date){ 3, 5, 0, 1, 30 };
    info.std_date = (struct __xbox_tz_date){ 10, 5, 0, 2, 0 };
    check_rule(&info, "<GMT>0<BST>-1,M3.5.0/1:30,M10.5.0/2", "rule: minutes + last week");

    /* An invalid date drops DST rather than producing a rule tzset() rejects. */
    info = zone(480, 0, -60, "PST", "PDT");
    info.dst_date = (struct __xbox_tz_date){ 13, 2, 0, 2, 0 };
    info.std_date = (struct __xbox_tz_date){ 11, 1, 0, 2, 0 };
    check_rule(&info, "<PST>8", "rule: invalid month drops DST");

    info = zone(480, 0, -60, "PST", "PDT");
    info.dst_date = (struct __xbox_tz_date){ 3, 2, 0, 2, 0 };
    info.std_date = (struct __xbox_tz_date){ 11, 1, 0, 2, 0 };
    CHECK_EQI(__xbox_tz_rule(&info, small, sizeof small), -1, "rule: too small a buffer fails");
    CHECK_EQI(__xbox_tz_rule(&info, small, 0), -1, "rule: zero-size buffer fails");

    /* What the builder writes, tzset() must accept. */
    info = zone(-600, 0, -60, "AEST", "AEDT");
    info.dst_date = (struct __xbox_tz_date){ 10, 1, 0, 2, 0 };
    info.std_date = (struct __xbox_tz_date){ 4, 1, 0, 3, 0 };
    char buf[__XBOX_TZ_RULE_MAX];
    CHECK(__xbox_tz_rule(&info, buf, sizeof buf) > 0, "rule: build Sydney");
    use_tz(buf);
    check_local(1705320000, 23, 1, 11 * 3600, "AEDT", "built rule round-trips through tzset");
}

int main(void)
{
    char *saved = NULL;
    const char *tz = getenv("TZ");
    if (tz)
        saved = strdup(tz);

    section_console_default();
    section_utc();
    section_rules();
    section_rule_builder();

    if (saved) {
        setenv("TZ", saved, 1);
        free(saved);
    } else {
        unsetenv("TZ");
    }
    tzset();

    CHECK_DONE("timezone");
    return 0;
}
