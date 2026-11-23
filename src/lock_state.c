/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lock_state.h"

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define FAILED_BANNER_MS 2000
#define MAX_LOCKOUT_SEC 10

int64_t lock_monotonic_ms(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static void wipe_picker(struct lock_state *st) {
	explicit_bzero(st->pick_items, sizeof(st->pick_items));
	explicit_bzero(st->pick_token, sizeof(st->pick_token));
	st->pick_count = 0;
	st->pick_selected = 0;
}

void lock_state_init(struct lock_state *st) {
	if (!st)
		return;
	memset(st, 0, sizeof(*st));
	st->ui_state = LOCK_UI_IDLE;
	st->last_reported_sec = -1;
	if (mlock(st->buf, sizeof(st->buf)) == 0)
		st->mlocked = true;
}

void lock_state_destroy(struct lock_state *st) {
	if (!st)
		return;
	explicit_bzero(st->buf, sizeof(st->buf));
	st->len = 0;
	st->char_count = 0;
	wipe_picker(st);
	if (st->mlocked) {
		(void)munlock(st->buf, sizeof(st->buf));
		st->mlocked = false;
	}
}

static bool is_locked_out(const struct lock_state *st, int64_t now_ms) {
	return st->ui_state == LOCK_UI_LOCKOUT && now_ms < st->state_until_ms;
}

bool lock_state_append_utf8(struct lock_state *st, const char *utf8,
							size_t nbytes, int64_t now_ms) {
	if (!st || !utf8 || nbytes == 0)
		return false;
	if (st->ui_state == LOCK_UI_VERIFYING || st->ui_state == LOCK_UI_PICKER ||
		is_locked_out(st, now_ms))
		return false;

	/* Reject ASCII control characters. */
	if (nbytes == 1) {
		unsigned char c = (unsigned char)utf8[0];
		if (c < 0x20 || c == 0x7f)
			return false;
	}

	if (st->len + nbytes > TAM_HIDE_MAX_PASS_BYTES)
		return false;

	memcpy(st->buf + st->len, utf8, nbytes);
	st->len += nbytes;
	st->buf[st->len] = '\0';
	st->char_count++;
	st->ui_state = LOCK_UI_TYPING;
	st->state_until_ms = 0;
	return true;
}

bool lock_state_backspace(struct lock_state *st, int64_t now_ms) {
	if (!st)
		return false;
	if (st->ui_state == LOCK_UI_VERIFYING || st->ui_state == LOCK_UI_PICKER ||
		is_locked_out(st, now_ms))
		return false;
	if (st->len == 0)
		return false;

	size_t pos = st->len - 1;
	/* Walk back over UTF-8 continuation bytes (10xxxxxx). */
	while (pos > 0 && (((unsigned char)st->buf[pos] & 0xC0) == 0x80))
		pos--;

	explicit_bzero(st->buf + pos, st->len - pos);
	st->len = pos;
	st->buf[st->len] = '\0';
	if (st->char_count > 0)
		st->char_count--;
	st->ui_state = (st->len > 0) ? LOCK_UI_TYPING : LOCK_UI_IDLE;
	return true;
}

bool lock_state_clear(struct lock_state *st, int64_t now_ms) {
	if (!st)
		return false;
	if (st->ui_state == LOCK_UI_VERIFYING || is_locked_out(st, now_ms))
		return false;

	bool changed = (st->len > 0 || st->ui_state != LOCK_UI_IDLE || st->pick_count > 0);
	explicit_bzero(st->buf, sizeof(st->buf));
	st->len = 0;
	st->char_count = 0;
	wipe_picker(st);
	st->ui_state = LOCK_UI_IDLE;
	st->state_until_ms = 0;
	return changed;
}

bool lock_state_can_submit(const struct lock_state *st, int64_t now_ms) {
	if (!st)
		return false;
	if (st->ui_state == LOCK_UI_VERIFYING || st->ui_state == LOCK_UI_PICKER ||
		is_locked_out(st, now_ms))
		return false;
	return st->len > 0;
}

bool lock_state_begin_verify(struct lock_state *st, char *out_buf,
							 size_t out_size, size_t *out_len) {
	if (!st || !out_buf || !out_len || st->len == 0)
		return false;
	if (st->len + 1 > out_size)
		return false;

	memcpy(out_buf, st->buf, st->len);
	out_buf[st->len] = '\0';
	*out_len = st->len;

	explicit_bzero(st->buf, sizeof(st->buf));
	st->len = 0;
	st->char_count = 0;
	st->ui_state = LOCK_UI_VERIFYING;
	st->state_until_ms = 0;
	return true;
}

void lock_state_on_auth_result(struct lock_state *st, bool ok, int64_t now_ms) {
	if (!st)
		return;
	explicit_bzero(st->buf, sizeof(st->buf));
	st->len = 0;
	st->char_count = 0;
	wipe_picker(st);

	if (ok) {
		st->fail_count = 0;
		st->ui_state = LOCK_UI_IDLE;
		st->state_until_ms = 0;
		st->last_reported_sec = -1;
		return;
	}

	st->fail_count++;
	if (st->fail_count >= 3) {
		unsigned int shift = st->fail_count - 3;
		int64_t delay_sec = (shift >= 4) ? MAX_LOCKOUT_SEC : ((int64_t)1 << shift);
		if (delay_sec > MAX_LOCKOUT_SEC)
			delay_sec = MAX_LOCKOUT_SEC;
		st->ui_state = LOCK_UI_LOCKOUT;
		st->state_until_ms = now_ms + delay_sec * 1000;
		st->last_reported_sec = (int)delay_sec;
	} else {
		st->ui_state = LOCK_UI_FAILED;
		st->state_until_ms = now_ms + FAILED_BANNER_MS;
		st->last_reported_sec = -1;
	}
}

void lock_state_on_lockout(struct lock_state *st, int lockout_sec, int64_t now_ms) {
	if (!st)
		return;
	explicit_bzero(st->buf, sizeof(st->buf));
	st->len = 0;
	st->char_count = 0;
	wipe_picker(st);

	if (lockout_sec < 1)
		lockout_sec = 1;
	if (lockout_sec > MAX_LOCKOUT_SEC)
		lockout_sec = MAX_LOCKOUT_SEC;
	st->ui_state = LOCK_UI_LOCKOUT;
	st->state_until_ms = now_ms + (int64_t)lockout_sec * 1000;
	st->last_reported_sec = lockout_sec;
}

bool lock_state_enter_picker(struct lock_state *st, const char *token,
							 const struct lock_pick_item *items, size_t count) {
	if (!st || !token || token[0] == '\0' || !items || count == 0 ||
		count > TAM_HIDE_MAX_PICK_ITEMS)
		return false;

	explicit_bzero(st->buf, sizeof(st->buf));
	st->len = 0;
	st->char_count = 0;
	wipe_picker(st);

	memcpy(st->pick_items, items, count * sizeof(*items));
	st->pick_count = count;
	st->pick_selected = 0;
	snprintf(st->pick_token, sizeof(st->pick_token), "%s", token);
	st->fail_count = 0;
	st->ui_state = LOCK_UI_PICKER;
	st->state_until_ms = 0;
	st->last_reported_sec = -1;
	return true;
}

bool lock_state_picker_move(struct lock_state *st, int delta) {
	if (!st || st->ui_state != LOCK_UI_PICKER || st->pick_count == 0)
		return false;
	if (delta == 0)
		return false;
	size_t n = st->pick_count;
	int next = ((int)st->pick_selected + delta) % (int)n;
	if (next < 0)
		next += (int)n;
	if ((size_t)next == st->pick_selected)
		return false;
	st->pick_selected = (size_t)next;
	return true;
}

bool lock_state_picker_select_digit(struct lock_state *st, int digit_1_based) {
	if (!st || st->ui_state != LOCK_UI_PICKER || st->pick_count == 0)
		return false;
	if (digit_1_based < 1 || (size_t)digit_1_based > st->pick_count)
		return false;
	st->pick_selected = (size_t)(digit_1_based - 1);
	return true;
}

bool lock_state_picker_cancel(struct lock_state *st) {
	if (!st || st->ui_state != LOCK_UI_PICKER)
		return false;
	wipe_picker(st);
	st->ui_state = LOCK_UI_IDLE;
	st->state_until_ms = 0;
	return true;
}

const struct lock_pick_item *lock_state_picker_current(const struct lock_state *st) {
	if (!st || st->ui_state != LOCK_UI_PICKER || st->pick_count == 0 ||
		st->pick_selected >= st->pick_count)
		return NULL;
	return &st->pick_items[st->pick_selected];
}

int lock_state_lockout_remaining_sec(const struct lock_state *st, int64_t now_ms) {
	if (!st || st->ui_state != LOCK_UI_LOCKOUT)
		return 0;
	if (now_ms >= st->state_until_ms)
		return 0;
	int64_t rem_ms = st->state_until_ms - now_ms;
	return (int)((rem_ms + 999) / 1000);
}

bool lock_state_tick(struct lock_state *st, int64_t now_ms) {
	if (!st)
		return false;

	if (st->ui_state == LOCK_UI_FAILED && now_ms >= st->state_until_ms) {
		st->ui_state = (st->len > 0) ? LOCK_UI_TYPING : LOCK_UI_IDLE;
		st->state_until_ms = 0;
		return true;
	}

	if (st->ui_state == LOCK_UI_LOCKOUT) {
		int sec = lock_state_lockout_remaining_sec(st, now_ms);
		if (sec <= 0) {
			st->ui_state = LOCK_UI_IDLE;
			st->state_until_ms = 0;
			st->last_reported_sec = -1;
			return true;
		}
		if (sec != st->last_reported_sec) {
			st->last_reported_sec = sec;
			return true;
		}
	}

	return false;
}

void lock_state_status_text(const struct lock_state *st, int64_t now_ms,
							char *out, size_t out_size) {
	if (!out || out_size == 0)
		return;
	if (!st) {
		out[0] = '\0';
		return;
	}

	switch (st->ui_state) {
	case LOCK_UI_VERIFYING:
		snprintf(out, out_size, "Verifying...");
		break;
	case LOCK_UI_FAILED:
		snprintf(out, out_size, "Incorrect");
		break;
	case LOCK_UI_LOCKOUT: {
		int sec = lock_state_lockout_remaining_sec(st, now_ms);
		if (sec > 0)
			snprintf(out, out_size, "Wait %ds", sec);
		else
			out[0] = '\0';
		break;
	}
	case LOCK_UI_PICKER:
		snprintf(out, out_size, "Up/Down or 1-9, Enter to unlock, Esc to cancel");
		break;
	case LOCK_UI_IDLE:
	case LOCK_UI_TYPING:
	default:
		out[0] = '\0';
		break;
	}
}
