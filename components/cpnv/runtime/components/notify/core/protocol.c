/* SPDX-License-Identifier: GPL-2.0-only */
#include "refnotify.h"
#include <errno.h>
#include <string.h>
uint32_t rn_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
void rn_put_le32(uint8_t *p, uint32_t v)
{
	for (unsigned i = 0; i < 4; ++i)
		p[i] = (uint8_t)(v >> (8u * i));
}
int rn_decode(const uint8_t *b, size_t n, struct rn_message *m)
{
	if (!b || !m)
		return -EINVAL;
	memset(m, 0, sizeof(*m));
	if (n < 8 || n > RN_MAX_FRAME)
		return -EMSGSIZE;
	if (rn_get_le32(b + 4) != n)
		return -EBADMSG;
	m->command = rn_get_le32(b);
	switch (m->command) {
	case 1:
		if (n != 11 && n != 12)
			return -EMSGSIZE;
		m->data.time.second = b[8];
		m->data.time.minute = b[9];
		m->data.time.hour = b[10];
		if (b[8] > 59 || b[9] > 59 || b[10] > 23)
			return -ERANGE;
		break;
	case 2:
		if (n != 13 && n != 14)
			return -EMSGSIZE;
		m->data.date.day = b[8];
		m->data.date.month = b[9];
		m->data.date.year = (unsigned)b[10] | (unsigned)b[11] << 8;
		m->data.date.weekday = b[12];
		{
			struct rn_calendar c = {
			    .year = (int)m->data.date.year, .month = (int)b[9], .day = (int)b[8]};
			int w, ret;
			int64_t e;
			ret = rn_calendar_epoch(&c, &e, &w);
			if (ret)
				return ret;
			if (b[12] > 6 || (int)b[12] != w)
				return -ERANGE;
		}
		break;
	case 3:
	case 4:
	case 5:
	case 8:
	case 9:
		if (n != 8)
			return -EMSGSIZE;
		break;
	case 10:
		if (n != 12)
			return -EMSGSIZE;
		m->data.cp_counter = rn_get_le32(b + 8);
		break;
	default:
		return -EOPNOTSUPP; /* Includes inbound display-only 6/7. */
	}
	return 0;
}
void rn_policy_readonly(struct rn_policy *p)
{
	memset(p, 0, sizeof(*p));
	p->query_time = p->query_date = p->time_association = true;
}
static int checked(int ret)
{
	return ret > 0 || ret < -4095 ? -EPROTO : ret;
}
static void header(uint8_t *b, uint32_t cmd, uint32_t len)
{
	rn_put_le32(b, cmd);
	rn_put_le32(b + 4, len);
}
int rn_dispatch(const uint8_t *b, size_t n, const struct rn_policy *p, const struct rn_ops *ops,
		void *ctx, struct rn_result *r)
{
	struct rn_message m = {0};
	struct rn_calendar c;
	int ret, weekday;
	int64_t epoch;
	uint64_t up;
	bool held = false;
	if (!r)
		return -EINVAL;
	memset(r, 0, sizeof(*r));
	if (!p || !ops) {
		r->error = -EINVAL;
		return r->error;
	}
	ret = rn_decode(b, n, &m);
	r->command = m.command;
	if (ret)
		goto done;
	switch (m.command) {
	case 5:
		ret = p->reboot_download ? -EOPNOTSUPP : -EPERM;
		goto done;
	case 8:
		ret = p->reboot_normal ? -EOPNOTSUPP : -EPERM;
		goto done;
	case 9:
		ret = p->diagnostic_iq ? -EOPNOTSUPP : -EPERM;
		goto done;
	case 10:
		if (!p->time_association) {
			ret = -EPERM;
			goto done;
		}
		if (!ops->uptime) {
			ret = -EOPNOTSUPP;
			goto done;
		}
		ret = checked(ops->uptime(ctx, &up));
		if (ret)
			goto done;
		rn_put_le32(r->association, m.data.cp_counter);
		rn_put_le32(r->association + 4, (uint32_t)up);
		r->association_valid = true;
		goto done;
	case 1:
	case 2:
		if (!(m.command == 1 ? p->set_time : p->set_date)) {
			ret = -EPERM;
			goto done;
		}
		if (!p->rtc_is_utc || !ops->hold || !ops->drop || !ops->write_calendar ||
		    !ops->write_clock) {
			ret = -EOPNOTSUPP;
			goto done;
		}
		if (!ops->read_calendar) {
			ret = -EOPNOTSUPP;
			goto done;
		}
		ret = checked(ops->hold(ctx));
		if (ret)
			goto done;
		held = true;
		break;
	case 3:
		if (!p->query_time) {
			ret = -EPERM;
			goto done;
		}
		break;
	case 4:
		if (!p->query_date) {
			ret = -EPERM;
			goto done;
		}
		break;
	default:
		ret = -EOPNOTSUPP;
		goto done;
	}
	if (!ops->read_calendar) {
		ret = -EOPNOTSUPP;
		goto done;
	}
	memset(&c, 0, sizeof(c));
	ret = checked(ops->read_calendar(ctx, &c));
	if (ret)
		goto done;
	ret = rn_calendar_epoch(&c, &epoch, &weekday);
	if (ret)
		goto done;
	if (m.command == 3) {
		header(r->reply, 3, 11);
		r->reply[8] = (uint8_t)c.second;
		r->reply[9] = (uint8_t)c.minute;
		r->reply[10] = (uint8_t)c.hour;
		r->reply_len = 11;
	} else if (m.command == 4) {
		header(r->reply, 4, 14);
		r->reply[8] = (uint8_t)c.day;
		r->reply[9] = (uint8_t)c.month;
		r->reply[10] = (uint8_t)c.year;
		r->reply[11] = (uint8_t)((unsigned)c.year >> 8);
		r->reply[12] = (uint8_t)weekday;
		r->reply[13] = 0;
		r->reply_len = 14;
	} else {
		if (m.command == 1) {
			c.second = (int)m.data.time.second;
			c.minute = (int)m.data.time.minute;
			c.hour = (int)m.data.time.hour;
		} else {
			c.year = (int)m.data.date.year;
			c.month = (int)m.data.date.month;
			c.day = (int)m.data.date.day;
		}
		ret = rn_calendar_epoch(&c, &epoch, &weekday);
		if (ret)
			goto done;
		c.weekday = weekday;
		r->rtc_attempted = true;
		ret = checked(ops->write_calendar(ctx, &c));
		if (ret)
			goto done;
		r->rtc_committed = true;
		r->clock_attempted = true;
		ret = checked(ops->write_clock(ctx, epoch));
		if (ret)
			goto done;
		r->clock_committed = true;
	}
done:
	if (held)
		ops->drop(ctx);
	r->error = ret;
	return ret;
}
int rn_encode_display_event(unsigned cmd, uint8_t out[8])
{
	if (!out)
		return -EINVAL;
	memset(out, 0, 8);
	if (cmd != 6 && cmd != 7)
		return -EOPNOTSUPP;
	header(out, cmd, 8);
	return 0;
}
