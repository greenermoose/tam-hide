/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "power_policy.h"

#include <assert.h>
#include <stdio.h>

static void test_parse_seconds(void) {
	uint32_t ms = 999;

	assert(power_policy_parse_seconds("0", &ms));
	assert(ms == 0);

	assert(power_policy_parse_seconds("1", &ms));
	assert(ms == 1000);

	assert(power_policy_parse_seconds("30", &ms));
	assert(ms == 30000);

	assert(power_policy_parse_seconds("86400", &ms));
	assert(ms == 86400000U);

	assert(!power_policy_parse_seconds(NULL, &ms));
	assert(!power_policy_parse_seconds("", &ms));
	assert(!power_policy_parse_seconds("-1", &ms));
	assert(!power_policy_parse_seconds("+5", &ms));
	assert(!power_policy_parse_seconds(" 5", &ms));
	assert(!power_policy_parse_seconds("10s", &ms));
	assert(!power_policy_parse_seconds("abc", &ms));
	assert(!power_policy_parse_seconds("86401", &ms));
	assert(!power_policy_parse_seconds("99999999999999999999999", &ms));
}

static void test_graceful_degradation(void) {
	struct power_policy p;

	/* Disabled by timeout=0. */
	power_policy_init(&p, 0);
	assert(!power_policy_on_locked(&p, true, true));
	assert(!power_policy_on_idle(&p, false));
	assert(!p.blanked);

	/* Missing ext_idle_notifier_v1 / wl_seat. */
	power_policy_init(&p, 30000);
	assert(!power_policy_on_locked(&p, false, true));
	assert(!power_policy_on_idle(&p, false));

	/* Missing zwlr_output_power_manager_v1 still arms software black blanking. */
	power_policy_init(&p, 30000);
	assert(power_policy_on_locked(&p, true, false));
	assert(power_policy_on_idle(&p, false));
	assert(p.blanked);
	assert(power_policy_on_activity(&p));
	assert(!p.blanked);
}

static void test_blank_wake_and_unlock(void) {
	struct power_policy p;
	power_policy_init(&p, 30000);

	/* Never blank before locked. */
	assert(!power_policy_on_idle(&p, false));

	/* Armed on locked when both protocols exist. */
	assert(power_policy_on_locked(&p, true, true));
	assert(!p.blanked);

	/* Activity while awake is a no-op. */
	assert(!power_policy_on_activity(&p));

	/* Never blank mid-verification. */
	assert(!power_policy_on_idle(&p, true));
	assert(!p.blanked);

	/* Idle when not verifying blanks once. */
	assert(power_policy_on_idle(&p, false));
	assert(p.blanked);
	assert(!power_policy_on_idle(&p, false)); /* duplicate idled is a no-op */

	/* Activity wakes once. */
	assert(power_policy_on_activity(&p));
	assert(!p.blanked);
	assert(!power_policy_on_activity(&p)); /* duplicate resumed is a no-op */

	/* Blank again, then unlock while blanked must request power-on. */
	assert(power_policy_on_idle(&p, false));
	assert(p.blanked);
	assert(power_policy_on_unlock(&p));
	assert(!p.blanked);
	assert(!p.locked);
	assert(!p.enabled);

	/* Unlock while awake does not request redundant power-on. */
	power_policy_init(&p, 30000);
	assert(power_policy_on_locked(&p, true, true));
	assert(!power_policy_on_unlock(&p));
}

int main(void) {
	test_parse_seconds();
	test_graceful_degradation();
	test_blank_wake_and_unlock();
	printf("PASS: test_power_policy\n");
	return 0;
}
