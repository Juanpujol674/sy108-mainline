/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CPNV_LIFECYCLE_H
#define CPNV_LIFECYCLE_H
#include <stdint.h>
/* Local child identities and exit observations. */
#define LC_ROLES 5u
enum lc_phase { LC_EXIT_WAIT };
enum lc_effect { LC_NO_EFFECT, LC_APPLIED };
enum lc_code { LC_OK = 0, LC_PENDING = 1 };
struct lc_identity {
	uint64_t token;
	uint32_t pid;
};
struct lc_completion {
	uint64_t sequence;
	int result;
	enum lc_effect effect;
	unsigned exited_mask;
	uint64_t exit_token[LC_ROLES];
};
struct lc_state {
	uint64_t epoch, sequence;
	enum lc_phase phase;
	unsigned registered_mask;
	struct lc_identity roles[LC_ROLES];
};
#endif
