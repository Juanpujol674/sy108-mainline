/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef UDX_HALO_PEER_GATE_H
#define UDX_HALO_PEER_GATE_H
/* AP-local bounded scheduling policy, not a vendor credential rule.
 * Caller selects total HALO phase budget before mc_deadline; no renewal. */
#define MC_HALO_TOTAL_MAX_MS 120000u
#define MC_HALO_REQUERY_MS 30000u
struct mc_runtime;
/* Original AP code waits 30 seconds and queries again for every nonzero raw
 * peer-state byte. This bounded adaptation uses the existing runtime guard,
 * lease/session monitoring and absolute deadline; it never extends deadline.
 * Return 0 means only that a complete query observed raw zero, not credentials,
 * entitlement, CP readiness or atomic stream freshness. No mark command is
 * sent. On failure *raw is cleared and caller must not consume it.
 * No private operation or policy acceptance occurs here. */
int mc_halo_wait_peer_zero(struct mc_runtime *, unsigned *raw);
#endif
