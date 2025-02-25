/* $OpenBSD: a_time_posix.c,v 1.2 2022/11/08 14:42:42 beck Exp $ */
/*
 * Copyright (c) 2022, Google Inc.
 * Copyright (c) 2022, Bob Beck <beck@obtuse.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Time conversion to/from POSIX time_t and struct tm, with no support
 * for time zones other than UTC
 */

#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <time.h>

#define SECS_PER_HOUR (int64_t)(60 * 60)
#define SECS_PER_DAY (int64_t)(24 * SECS_PER_HOUR)

/*
 * Is a year/month/day combination valid, in the range from year 0000
 * to 9999?
 */
static int
is_valid_date(int year, int month, int day)
{
	int days_in_month;
	if (day < 1 || month < 1 || year < 0 || year > 9999)
		return 0;
	switch (month) {
	case 1:
	case 3:
	case 5:
	case 7:
	case 8:
	case 10:
	case 12:
		days_in_month = 31;
		break;
	case 4:
	case 6:
	case 9:
	case 11:
		days_in_month = 30;
		break;
	case 2:
		if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)
			days_in_month = 29;
		else
			days_in_month = 28;
		break;
	default:
		return 0;
	}
	return day <= days_in_month;
}

/*
 * Is a time valid? Leap seconds of 60 are not considered valid, as
 * the POSIX time in seconds does not include them.
 */
static int
is_valid_time(int hours, int minutes, int seconds)
{
	return hours >= 0 && minutes >= 0 && seconds >= 0 && hours <= 23 &&
	    minutes <= 59 && seconds <= 59;
}

/* Is a int64 time representing a time within our expected range? */
static int
is_valid_epoch_time(int64_t time)
{
	/* 0000-01-01 00:00:00 UTC to 9999-12-31 23:59:59 UTC */
	return (int64_t)-62167219200 <= time && time <= (int64_t)253402300799;
}

/*
 * Inspired by algorithms presented in
 * https://howardhinnant.github.io/date_algorithms.html
 * (Public Domain)
 */
static int
posix_time_from_utc(int year, int month, int day, int hours,  int minutes,
    int seconds, int64_t *out_time)
{
	int64_t era, year_of_era, day_of_year, day_of_era, posix_days;

	if (!is_valid_date(year, month, day) ||
	    !is_valid_time(hours, minutes, seconds))
		return 0;
	if (month <= 2)
		year--;  /* Start years on Mar 1, so leap days end a year. */

	/* At this point year will be in the range -1 and 9999.*/
	era = (year >= 0 ? year : year - 399) / 400;
	year_of_era = year - era * 400;
	day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) /
	    5 + day - 1;
	day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era /
	    100 + day_of_year;
	posix_days = era * 146097 + day_of_era - 719468;
	*out_time = posix_days * SECS_PER_DAY + hours * SECS_PER_HOUR +
	    minutes * 60 + seconds;

	return 1;
}

/*
 * Inspired by algorithms presented in
 * https://howardhinnant.github.io/date_algorithms.html
 * (Public Domain)
 */
static int
utc_from_posix_time(int64_t time, int *out_year, int *out_month, int *out_day,
    int *out_hours, int *out_minutes, int *out_seconds)
{
	int64_t days, leftover_seconds, era, day_of_era, year_of_era,
	    day_of_year, month_of_year;

	if (!is_valid_epoch_time(time))
		return 0;

	days = time / SECS_PER_DAY;
	leftover_seconds = time % SECS_PER_DAY;
	if (leftover_seconds < 0) {
		days--;
		leftover_seconds += SECS_PER_DAY;
	}
	days += 719468;  /*  Shift to starting epoch of Mar 1 0000. */

	/* At this point, days will be in the range -61 and 3652364. */
	era = (days > 0 ? days : days - 146096) / 146097;
	day_of_era = days - era * 146097;
	year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 -
	    day_of_era / 146096) /
	    365;
	*out_year = year_of_era + era * 400;  /* Year starts on Mar 1 */
	day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 -
	    year_of_era / 100);
	month_of_year = (5 * day_of_year + 2) / 153;
	*out_month = (month_of_year < 10 ? month_of_year + 3 :
	    month_of_year - 9);
	if (*out_month <= 2)
		(*out_year)++;  /* Adjust year back to Jan 1 start of year. */

	*out_day = day_of_year - (153 * month_of_year + 2) / 5 + 1;
	*out_hours = leftover_seconds / SECS_PER_HOUR;
	leftover_seconds %= SECS_PER_HOUR;
	*out_minutes = leftover_seconds / 60;
	*out_seconds = leftover_seconds % 60;

	return 1;
}

static int
asn1_time_tm_to_posix(const struct tm *tm, int64_t *out)
{
	/* Ensure additions below do not overflow */
	if (tm->tm_year > 9999)
		return 0;
	if (tm->tm_mon > 12)
		return 0;

	return posix_time_from_utc(tm->tm_year + 1900, tm->tm_mon + 1,
	    tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, out);
}

static int
asn1_time_posix_to_tm(int64_t time, struct tm *out_tm)
{
	memset(out_tm, 0, sizeof(struct tm));
	if (!utc_from_posix_time(time, &out_tm->tm_year, &out_tm->tm_mon,
	    &out_tm->tm_mday, &out_tm->tm_hour, &out_tm->tm_min,
	    &out_tm->tm_sec))
		return 0;

	out_tm->tm_year -= 1900;
	out_tm->tm_mon -= 1;

	return 1;
}

int
asn1_time_tm_to_time_t(const struct tm *tm, time_t *out)
{
	int64_t posix_time;

	if (!asn1_time_tm_to_posix(tm, &posix_time))
		return 0;

#ifdef SMALL_TIME_T
	/* For portable. */
	if (sizeof(time_t) == sizeof(int32_t) &&
	    (posix_time > INT32_MAX || posix_time < INT32_MIN))
		return 0;
#endif

	*out = posix_time;
	return 1;
}

int
asn1_time_time_t_to_tm(const time_t *time, struct tm *out_tm)
{
	int64_t posix_time = *time;

	return asn1_time_posix_to_tm(posix_time, out_tm);
}

int
OPENSSL_gmtime_adj(struct tm *tm, int off_day, long offset_sec)
{
	int64_t posix_time;

	/* Ensure additions below do not overflow */
	if (tm->tm_year > 9999)
		return 0;
	if (tm->tm_mon > 12)
		return 0;

	if (!posix_time_from_utc(tm->tm_year + 1900, tm->tm_mon + 1,
	    tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, &posix_time))
		return 0;

	if (!utc_from_posix_time(posix_time + off_day * SECS_PER_DAY +
	    offset_sec, &tm->tm_year, &tm->tm_mon, &tm->tm_mday, &tm->tm_hour,
	    &tm->tm_min, &tm->tm_sec))
		return 0;

	tm->tm_year -= 1900;
	tm->tm_mon -= 1;

	return 1;
}

int
OPENSSL_gmtime_diff(int *out_days, int *out_secs, const struct tm *from,
    const struct tm *to)
{
	int64_t time_to, time_from, timediff, daydiff;

	if (!posix_time_from_utc(to->tm_year + 1900, to->tm_mon + 1,
	    to->tm_mday, to->tm_hour, to->tm_min, to->tm_sec, &time_to))
		return 0;

	if (!posix_time_from_utc(from->tm_year + 1900, from->tm_mon + 1,
	    from->tm_mday, from->tm_hour, from->tm_min,
	    from->tm_sec, &time_from))
		return 0;

	timediff = time_to - time_from;
	daydiff = timediff / SECS_PER_DAY;
	timediff %= SECS_PER_DAY;
	if (daydiff > INT_MAX || daydiff < INT_MIN)
		return 0;

	*out_secs = timediff;
	*out_days = daydiff;

	return 1;
}
