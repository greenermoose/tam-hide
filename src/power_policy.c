/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "power_policy.h"

#include <errno.h>
#include <stdlib.h>

void power_policy_init(struct power_policy *p, uint32_t timeout_ms) {
	if (!p)
		return;
	p->timeout_ms = timeout_ms;
	p->locked = false;
	p->enabled = false;
	p->blanked = false;
}

bool power_policy_parse_seconds(const char *arg, uint32_t *out_ms) {
	if (!arg || !*arg || !out_ms)
		return false;
	if (arg[0] < '0' || arg[0] > '9')
		return false;

	errno = 0;
	char *end = NULL;
	unsigned long long secs = strtoull(arg, &end, 10);
	if (errno != 0 || !end || *end != '\0')
		return false;
	if (secs > TAM_HIDE_MAX_BLANK_SEC)
		return false;

	*out_ms = (uint32_t)(secs * 1000ULL);
	return true;
}

bool power_policy_on_locked(struct power_policy *p, bool has_idle,
							bool has_power) {
	if (!p)
		return false;
	p->locked = true;
	p->enabled = (p->timeout_ms > 0 && has_idle && has_power);
	return p->enabled;
}

bool power_policy_on_idle(struct power_policy *p, bool verifying) {
	if (!p || !p->locked || !p->enabled || p->blanked || verifying)
		return false;
	p->blanked = true;
	return true;
}

bool power_policy_on_activity(struct power_policy *p) {
	if (!p || !p->blanked)
		return false;
	p->blanked = false;
	return true;
}

bool power_policy_on_unlock(struct power_policy *p) {
	if (!p)
		return false;
	p->enabled = false;
	p->locked = false;
	if (!p->blanked)
		return false;
	p->blanked = false;
	return true;
}
