/* SPDX-License-Identifier: GPL-2.0-only */
#include "refnotify.h"
#include <errno.h>
static bool leap(int year)
{
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
int rn_calendar_epoch(const struct rn_calendar *c, int64_t *epoch, int *weekday)
{
	static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int64_t n = 0;
	unsigned limit;
	if (!c || !epoch || !weekday)
		return -EINVAL;
	if (c->year < 1970 || c->year > 9999 || c->month < 1 || c->month > 12 || c->hour < 0 ||
	    c->hour > 23 || c->minute < 0 || c->minute > 59 || c->second < 0 || c->second > 59)
		return -ERANGE;
	limit = days[c->month - 1] + (unsigned)(c->month == 2 && leap(c->year));
	if (c->day < 1 || (unsigned)c->day > limit)
		return -ERANGE;
	for (int y = 1970; y < c->year; ++y)
		n += 365 + (int)leap(y);
	for (int m = 1; m < c->month; ++m)
		n += days[m - 1] + (unsigned)(m == 2 && leap(c->year));
	n += c->day - 1;
	*weekday = (int)((n + 4) % 7);
	*epoch = n * 86400 + c->hour * 3600 + c->minute * 60 + c->second;
	return 0;
}
