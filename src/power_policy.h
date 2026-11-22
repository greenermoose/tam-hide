/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_POWER_POLICY_H
#define TAM_HIDE_POWER_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define TAM_HIDE_DEFAULT_BLANK_SEC 30U
#define TAM_HIDE_DEFAULT_BLANK_MS (TAM_HIDE_DEFAULT_BLANK_SEC * 1000U)
#define TAM_HIDE_MAX_BLANK_SEC 86400U

struct power_policy {
	uint32_t timeout_ms;
	bool locked;
	bool enabled;
	bool blanked;
};

/* Initialize blanking policy with timeout in milliseconds (0 disables blanking). */
void power_policy_init(struct power_policy *p, uint32_t timeout_ms);

/*
 * Parse non-negative integer seconds for --blank-after.
 * 0 disables blanking; values > TAM_HIDE_MAX_BLANK_SEC are rejected.
 * Returns true on valid input and writes milliseconds to *out_ms.
 */
bool power_policy_parse_seconds(const char *arg, uint32_t *out_ms);

/*
 * Record that ext_session_lock_v1.locked fired.
 * Returns true if idle blanking should be armed (requires timeout_ms > 0,
 * ext_idle_notifier_v1 + wl_seat, and zwlr_output_power_manager_v1).
 */
bool power_policy_on_locked(struct power_policy *p, bool has_idle,
							bool has_power);

/*
 * Record ext_idle_notification_v1.idled.
 * Returns true if outputs should transition to OFF and partial input should be cleared.
 */
bool power_policy_on_idle(struct power_policy *p, bool verifying);

/*
 * Record user activity (ext_idle_notification_v1.resumed or keyboard fallback).
 * Returns true if outputs should transition back to ON and redraw.
 */
bool power_policy_on_activity(struct power_policy *p);

/*
 * Record impending unlock after AUTH_OK.
 * Returns true if outputs are blanked and must be restored to ON before unlock.
 */
bool power_policy_on_unlock(struct power_policy *p);

#endif
