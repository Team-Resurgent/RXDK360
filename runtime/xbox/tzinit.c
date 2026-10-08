/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/*
 * Default TZ from the console's time zone setting.
 *
 * picolibc's tzset() only reads the TZ environment variable and falls back to
 * UTC without it, so localtime() would ignore the time zone chosen in the
 * dashboard. Before the constructors run, _start calls this to build the
 * equivalent POSIX rule from xapilib's GetTimeZoneInformation() and install it
 * as TZ, unless the title already set one. The original Xbox runtime does the
 * same from its EEPROM settings; the rule itself comes from the shared
 * __xbox_tz_rule() in picolibc.
 */

/* setenv() is POSIX; picolibc hides it under a strict -std. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/xbox_tz.h>

/* xapilib's TIME_ZONE_INFORMATION (XDK winbase.h); declared here so the runtime
   needs no XDK header. LONG is 32 bits and WCHAR 16 under -fshort-wchar. */
typedef struct {
    unsigned short wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} xbox_systemtime;
typedef struct {
    long            Bias;
    unsigned short  StandardName[32];
    xbox_systemtime StandardDate;
    long            StandardBias;
    unsigned short  DaylightName[32];
    xbox_systemtime DaylightDate;
    long            DaylightBias;
} xbox_time_zone_information;

extern unsigned long GetTimeZoneInformation(xbox_time_zone_information *info);
#define TIME_ZONE_ID_INVALID 0xFFFFFFFFul

static void copy_name(char *dst, size_t size, const unsigned short *src, size_t count)
{
    size_t i;
    for (i = 0; i + 1 < size && i < count && src[i] != 0; i++)
        dst[i] = src[i] < 0x80 ? (char)src[i] : '?'; /* non-ASCII -> numeric name */
    dst[i] = '\0';
}

static void copy_date(struct __xbox_tz_date *dst, const xbox_systemtime *src)
{
    /* Only "Nth weekday of a month" rules have a POSIX form; a fixed-year date
       (wYear != 0) is left out, which drops daylight saving time. */
    if (src->wYear != 0)
        return;
    dst->month = (unsigned char)src->wMonth;
    dst->week = (unsigned char)src->wDay;
    dst->wday = (unsigned char)src->wDayOfWeek;
    dst->hour = (unsigned char)src->wHour;
    dst->minute = (unsigned char)src->wMinute;
}

void xbox_init_timezone(void)
{
    xbox_time_zone_information tzi;
    struct __xbox_tz_info      info;
    char                       rule[__XBOX_TZ_RULE_MAX];

    if (getenv("TZ"))
        return;

    memset(&tzi, 0, sizeof(tzi));
    if (GetTimeZoneInformation(&tzi) == TIME_ZONE_ID_INVALID)
        return;

    memset(&info, 0, sizeof(info));
    info.bias = tzi.Bias;
    info.std_bias = tzi.StandardBias;
    info.dst_bias = tzi.DaylightBias;
    copy_name(info.std_name, sizeof(info.std_name), tzi.StandardName, 32);
    copy_name(info.dst_name, sizeof(info.dst_name), tzi.DaylightName, 32);
    copy_date(&info.std_date, &tzi.StandardDate);
    copy_date(&info.dst_date, &tzi.DaylightDate);

    if (__xbox_tz_rule(&info, rule, sizeof(rule)) > 0)
        setenv("TZ", rule, 0);
}
