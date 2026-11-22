/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_LOCK_STATE_H
#define TAM_HIDE_LOCK_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TAM_HIDE_MAX_PASS_BYTES 256

enum lock_ui_state {
	LOCK_UI_IDLE = 0,
	LOCK_UI_TYPING = 1,
	LOCK_UI_VERIFYING = 2,
	LOCK_UI_FAILED = 3,
	LOCK_UI_LOCKOUT = 4,
};

struct lock_state {
	char buf[TAM_HIDE_MAX_PASS_BYTES + 1];
	size_t len;
	size_t char_count;
	enum lock_ui_state ui_state;
	unsigned int fail_count;
	int64_t state_until_ms;
	int last_reported_sec;
	bool mlocked;
};

/* Initialize state and best-effort mlock the password buffer. */
void lock_state_init(struct lock_state *st);

/* Wipe password buffer with explicit_bzero and munlock if locked. */
void lock_state_destroy(struct lock_state *st);

/* Append valid printable UTF-8 bytes; returns true if UI should redraw. */
bool lock_state_append_utf8(struct lock_state *st, const char *utf8,
							size_t nbytes, int64_t now_ms);

/* Delete the last UTF-8 codepoint; returns true if UI should redraw. */
bool lock_state_backspace(struct lock_state *st, int64_t now_ms);

/* Clear current input buffer; returns true if UI should redraw. */
bool lock_state_clear(struct lock_state *st, int64_t now_ms);

/* True if the state accepts a password submission right now. */
bool lock_state_can_submit(const struct lock_state *st, int64_t now_ms);

/*
 * Copy the password into out_buf, wipe st->buf immediately, and transition
 * to LOCK_UI_VERIFYING. Returns true on success.
 */
bool lock_state_begin_verify(struct lock_state *st, char *out_buf,
							 size_t out_size, size_t *out_len);

/* Update state after an authentication attempt completes. */
void lock_state_on_auth_result(struct lock_state *st, bool ok, int64_t now_ms);

/* Advance timers (FAILED banner expiry / LOCKOUT countdown). Returns true if redraw needed. */
bool lock_state_tick(struct lock_state *st, int64_t now_ms);

/* Remaining lockout seconds (0 if not in lockout). */
int lock_state_lockout_remaining_sec(const struct lock_state *st, int64_t now_ms);

/*
 * Format a strictly non-disclosing status string (never includes usernames,
 * hostnames, or session metadata).
 */
void lock_state_status_text(const struct lock_state *st, int64_t now_ms,
							char *out, size_t out_size);

/* Monotonic clock helper in milliseconds. */
int64_t lock_monotonic_ms(void);

#endif
