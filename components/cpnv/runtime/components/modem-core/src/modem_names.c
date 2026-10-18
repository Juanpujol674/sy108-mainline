/* SPDX-License-Identifier: GPL-2.0-only */
#include "cproc_backend.h"
const char *cpb_domain(unsigned d)
{
	static const char *const names[] = {"cptl", "v3phy", "nrphy", "pmic"};
	return d < 4 ? names[d] : 0;
}
const char *cpb_segment(unsigned d, unsigned s)
{
	static const char *const names[4][7] = {
	    {"modem", "v3phy", "nrphy", "nrdsp1", "nrdsp2", "deltanv", "cpcmdline"},
	    {"v3phy", 0, 0, 0, 0, 0, 0},
	    {"nrphy", "nrdsp1", "nrdsp2", 0, 0, 0, 0},
	    {"pm_sys", 0, 0, 0, 0, 0, 0}};
	return d < 4 && s < 7 ? names[d][s] : 0;
}
