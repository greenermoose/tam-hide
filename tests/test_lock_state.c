/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lock_state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_utf8_editing_and_wipe(void) {
	struct lock_state st;
	lock_state_init(&st);

	assert(st.ui_state == LOCK_UI_IDLE);
	assert(!lock_state_can_submit(&st, 1000));

	/* Control characters must be rejected. */
	assert(!lock_state_append_utf8(&st, "\x01", 1, 1000));
	assert(!lock_state_append_utf8(&st, "\x7f", 1, 1000));
	assert(st.len == 0);

	/* Append ASCII + multi-byte UTF-8 ("é" = 0xC3 0xA9, "🔒" = 4 bytes). */
	assert(lock_state_append_utf8(&st, "a", 1, 1000));
	assert(lock_state_append_utf8(&st, "\xC3\xA9", 2, 1000));
	assert(lock_state_append_utf8(&st, "\xF0\x9F\x94\x92", 4, 1000));
	assert(st.len == 7);
	assert(st.char_count == 3);
	assert(st.ui_state == LOCK_UI_TYPING);
	assert(lock_state_can_submit(&st, 1000));

	/* Backspace removes one full UTF-8 codepoint at a time. */
	assert(lock_state_backspace(&st, 1000));
	assert(st.len == 3);
	assert(st.char_count == 2);
	assert(memcmp(st.buf, "a\xC3\xA9", 3) == 0);

	assert(lock_state_backspace(&st, 1000));
	assert(st.len == 1);
	assert(st.char_count == 1);
	assert(strcmp(st.buf, "a") == 0);

	assert(lock_state_backspace(&st, 1000));
	assert(st.len == 0);
	assert(st.char_count == 0);
	assert(st.ui_state == LOCK_UI_IDLE);

	/* Clear wipes buffer completely. */
	assert(lock_state_append_utf8(&st, "secret", 6, 1000));
	assert(lock_state_clear(&st, 1000));
	assert(st.len == 0);
	for (size_t i = 0; i < sizeof(st.buf); i++)
		assert(st.buf[i] == 0);

	lock_state_destroy(&st);
}

static void test_max_length_and_verify_wipe(void) {
	struct lock_state st;
	lock_state_init(&st);

	for (int i = 0; i < TAM_HIDE_MAX_PASS_BYTES; i++)
		assert(lock_state_append_utf8(&st, "x", 1, 1000));
	assert(st.len == TAM_HIDE_MAX_PASS_BYTES);
	assert(!lock_state_append_utf8(&st, "y", 1, 1000));

	char out[TAM_HIDE_MAX_PASS_BYTES + 1];
	size_t out_len = 0;
	assert(lock_state_begin_verify(&st, out, sizeof(out), &out_len));
	assert(out_len == TAM_HIDE_MAX_PASS_BYTES);
	assert(st.ui_state == LOCK_UI_VERIFYING);
	assert(st.len == 0);
	assert(!lock_state_can_submit(&st, 1000));
	assert(!lock_state_append_utf8(&st, "z", 1, 1000));

	/* Internal buffer must be zeroed immediately on begin_verify. */
	for (size_t i = 0; i < sizeof(st.buf); i++)
		assert(st.buf[i] == 0);

	explicit_bzero(out, sizeof(out));
	lock_state_destroy(&st);
}

static void test_rate_limit_and_status_text(void) {
	struct lock_state st;
	lock_state_init(&st);
	char text[64];

	/* Failure 1 & 2 -> LOCK_UI_FAILED (no lockout yet). */
	lock_state_on_auth_result(&st, false, 1000);
	assert(st.ui_state == LOCK_UI_FAILED);
	lock_state_status_text(&st, 1000, text, sizeof(text));
	assert(strcmp(text, "Incorrect") == 0);
	assert(lock_state_tick(&st, 3000));
	assert(st.ui_state == LOCK_UI_IDLE);

	lock_state_on_auth_result(&st, false, 4000);
	assert(st.ui_state == LOCK_UI_FAILED);

	/* Failure 3 -> 1s lockout. */
	lock_state_on_auth_result(&st, false, 5000);
	assert(st.ui_state == LOCK_UI_LOCKOUT);
	assert(lock_state_lockout_remaining_sec(&st, 5000) == 1);
	assert(!lock_state_append_utf8(&st, "a", 1, 5500));
	lock_state_status_text(&st, 5100, text, sizeof(text));
	assert(strcmp(text, "Wait 1s") == 0);
	assert(lock_state_tick(&st, 6000));
	assert(st.ui_state == LOCK_UI_IDLE);

	/* Failure 4 -> 2s, Failure 5 -> 4s, Failure 6 -> 8s, Failure 7 -> 10s cap. */
	lock_state_on_auth_result(&st, false, 7000);
	assert(lock_state_lockout_remaining_sec(&st, 7000) == 2);
	lock_state_on_auth_result(&st, false, 10000);
	assert(lock_state_lockout_remaining_sec(&st, 10000) == 4);
	lock_state_on_auth_result(&st, false, 20000);
	assert(lock_state_lockout_remaining_sec(&st, 20000) == 8);
	lock_state_on_auth_result(&st, false, 30000);
	assert(lock_state_lockout_remaining_sec(&st, 30000) == 10);

	/* Success resets fail_count. */
	lock_state_on_auth_result(&st, true, 45000);
	assert(st.fail_count == 0);
	assert(st.ui_state == LOCK_UI_IDLE);

	lock_state_destroy(&st);
}

int main(void) {
	test_utf8_editing_and_wipe();
	test_max_length_and_verify_wipe();
	test_rate_limit_and_status_text();
	printf("test_lock_state: all tests passed\n");
	return 0;
}
