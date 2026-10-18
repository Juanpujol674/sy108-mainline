/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_WR_NOTIFY_H
#define UDX_WR_NOTIFY_H
#include "wr_parent.h"
int wr_notify_policy(struct wr_parent *);
int wr_notify_start(struct wr_parent *, struct wr_generation *, uint64_t deadline);
#endif
