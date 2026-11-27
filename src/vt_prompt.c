/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "vt_prompt.h"

#include <tam.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef TAM_HIDE_VERSION
#define TAM_HIDE_VERSION "0.3.0"
#endif

static void write_all(int fd, const char *buf, size_t len) {
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
}

static void wipe_pass(struct vt_prompt *p) {
	explicit_bzero(p->pass, sizeof(p->pass));
	p->pass_len = 0;
}

static void wipe_picker(struct vt_prompt *p) {
	explicit_bzero(p->pick_token, sizeof(p->pick_token));
	explicit_bzero(p->pick_items, sizeof(p->pick_items));
	p->pick_count = 0;
	p->pick_selected = 0;
	p->mode = VT_PROMPT_IDLE;
}

static size_t count_utf8_chars(const char *s, size_t len) {
	size_t count = 0;
	for (size_t i = 0; i < len; i++) {
		if (((unsigned char)s[i] & 0xC0u) != 0x80u)
			count++;
	}
	return count;
}

static void pop_utf8_char(struct vt_prompt *p) {
	if (p->pass_len == 0)
		return;
	while (p->pass_len > 0) {
		unsigned char c = (unsigned char)p->pass[p->pass_len - 1];
		p->pass[--p->pass_len] = '\0';
		if ((c & 0xC0u) != 0x80u)
			break;
	}
}

int vt_prompt_init_fd(struct vt_prompt *p, int fd, bool owns_fd) {
	if (!p || fd < 0)
		return -EINVAL;

	memset(p, 0, sizeof(*p));
	p->fd = fd;
	p->owns_fd = owns_fd;

	int flags = fcntl(fd, F_GETFL);
	if (flags >= 0)
		(void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	if (tcgetattr(fd, &p->saved_termios) == 0) {
		p->termios_saved = true;
		struct termios raw = p->saved_termios;
		raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
		raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		(void)tcsetattr(fd, TCSANOW, &raw);
	}

	p->active = true;
	vt_prompt_render(p);
	return 0;
}

int vt_prompt_init_vt(struct vt_prompt *p, int lock_vt) {
	if (!p || lock_vt < TAM_VT_MIN || lock_vt > TAM_VT_MAX)
		return -EINVAL;
	char path[32];
	snprintf(path, sizeof(path), "/dev/tty%d", lock_vt);
	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	int rc = vt_prompt_init_fd(p, fd, true);
	if (rc < 0) {
		close(fd);
		return rc;
	}
	return 0;
}

int vt_prompt_fd(const struct vt_prompt *p) {
	if (!p || !p->active)
		return -1;
	return p->fd;
}

void vt_prompt_render(struct vt_prompt *p) {
	if (!p || !p->active || p->fd < 0)
		return;

	char out[2048];
	size_t off = 0;
	int n = snprintf(out, sizeof(out),
					 "\033[2J\033[H\r\n  tam-hide %s\r\n", TAM_HIDE_VERSION);
	if (n > 0 && (size_t)n < sizeof(out))
		off = (size_t)n;

	if (p->mode == VT_PROMPT_PICKER) {
		n = snprintf(out + off, sizeof(out) - off,
					 "  Select session (1-%zu, Up/Down, Enter, Esc to cancel):\r\n\r\n",
					 p->pick_count);
		if (n > 0 && off + (size_t)n < sizeof(out))
			off += (size_t)n;

		for (size_t i = 0; i < p->pick_count; i++) {
			const struct seat_pick_candidate *it = &p->pick_items[i];
			const char *marker = (i == p->pick_selected) ? ">" : " ";
			if (it->vt_nr > 0) {
				n = snprintf(out + off, sizeof(out) - off,
							 "  %s %zu. %s (tty%d, %s)\r\n",
							 marker, i + 1, it->username, it->vt_nr,
							 session_kind_name(it->kind));
			} else {
				n = snprintf(out + off, sizeof(out) - off,
							 "  %s %zu. %s (no active VT)\r\n",
							 marker, i + 1, it->username);
			}
			if (n > 0 && off + (size_t)n < sizeof(out))
				off += (size_t)n;
		}
	} else {
		const char *st = (p->status[0] != '\0') ? p->status : "Seat locked";
		n = snprintf(out + off, sizeof(out) - off,
					 "  %s\r\n\r\n  Passcode: ", st);
		if (n > 0 && off + (size_t)n < sizeof(out))
			off += (size_t)n;

		size_t dots = count_utf8_chars(p->pass, p->pass_len);
		if (dots > 64)
			dots = 64;
		for (size_t i = 0; i < dots && off + 1 < sizeof(out); i++)
			out[off++] = '*';
		if (off + 2 < sizeof(out)) {
			out[off++] = '\r';
			out[off++] = '\n';
		}
	}

	write_all(p->fd, out, off);
	explicit_bzero(out, sizeof(out));
}

void vt_prompt_reset(struct vt_prompt *p, const char *status) {
	if (!p || !p->active)
		return;
	wipe_pass(p);
	wipe_picker(p);
	p->last_input_ms = 0;
	if (status)
		snprintf(p->status, sizeof(p->status), "%s", status);
	else
		p->status[0] = '\0';
	vt_prompt_render(p);
}

void vt_prompt_tick(struct vt_prompt *p, int64_t now_ms) {
	if (!p || !p->active)
		return;
	if (p->pass_len > 0 && p->last_input_ms > 0 &&
		now_ms - p->last_input_ms >= TAM_HIDE_VT_PROMPT_IDLE_CLEAR_MS) {
		wipe_pass(p);
		p->last_input_ms = 0;
		vt_prompt_render(p);
	}
}

static bool parse_pick_response(struct vt_prompt *p, char *resp) {
	wipe_picker(p);
	char *saveptr = NULL;
	char *line = strtok_r(resp, "\n", &saveptr);
	char tok[TAM_HIDE_PICK_TOKEN_HEX_LEN + 1];
	unsigned long expected = 0;
	if (!line || sscanf(line, "PICK token=%32s count=%lu", tok, &expected) != 2)
		return false;

	memcpy(p->pick_token, tok, sizeof(p->pick_token));
	size_t parsed = 0;
	while ((line = strtok_r(NULL, "\n", &saveptr)) != NULL) {
		if (strcmp(line, "END") == 0)
			break;
		if (parsed >= TAM_HIDE_MAX_PICK_CANDIDATES)
			continue;
		int idx = 0, vt = 0;
		unsigned uid = 0;
		char user[TAM_HIDE_USER_MAX], kind[16];
		if (sscanf(line, "ITEM %d user=%63s uid=%u vt=%d kind=%15s",
				   &idx, user, &uid, &vt, kind) == 5) {
			struct seat_pick_candidate *it = &p->pick_items[parsed++];
			it->index = idx;
			it->uid = (uid_t)uid;
			it->vt_nr = vt;
			snprintf(it->username, sizeof(it->username), "%s", user);
			it->kind = (strcmp(kind, "graphical") == 0)
						   ? SESSION_KIND_GRAPHICAL
						   : SESSION_KIND_CONSOLE;
		}
	}
	if (parsed == 0) {
		wipe_picker(p);
		return false;
	}
	p->pick_count = parsed;
	p->pick_selected = 0;
	p->mode = VT_PROMPT_PICKER;
	return true;
}

static void submit_passcode(struct vt_prompt *p, struct seat_controller *c) {
	if (p->pass_len == 0)
		return;

	char req[TAM_HIDE_VT_PROMPT_MAX_PASS + 16];
	memcpy(req, "AUTH ", 5);
	memcpy(req + 5, p->pass, p->pass_len);
	req[5 + p->pass_len] = '\n';
	req[5 + p->pass_len + 1] = '\0';
	wipe_pass(p);
	p->last_input_ms = 0;

	char resp[4096];
	(void)controller_dispatch_auth(c, c->allowed_uid, req, resp, sizeof(resp));
	explicit_bzero(req, sizeof(req));

	if (strncmp(resp, "OK ", 3) == 0) {
		p->status[0] = '\0';
		wipe_picker(p);
	} else if (strncmp(resp, "PICK ", 5) == 0) {
		p->status[0] = '\0';
		if (!parse_pick_response(p, resp))
			snprintf(p->status, sizeof(p->status), "Authentication failed");
	} else if (strncmp(resp, "ERR lockout sec=", 16) == 0) {
		int sec = atoi(resp + 16);
		if (sec < 1)
			sec = 1;
		snprintf(p->status, sizeof(p->status), "Locked out (%ds)", sec);
	} else {
		snprintf(p->status, sizeof(p->status), "Authentication failed");
	}

	explicit_bzero(resp, sizeof(resp));
	if (c->state == SEAT_STATE_LOCKED)
		vt_prompt_render(p);
}

static void submit_pick(struct vt_prompt *p, struct seat_controller *c) {
	if (p->mode != VT_PROMPT_PICKER || p->pick_selected >= p->pick_count)
		return;

	char req[128];
	snprintf(req, sizeof(req), "SELECT %s %d\n", p->pick_token,
			 p->pick_items[p->pick_selected].index);
	char resp[256];
	(void)controller_dispatch_auth(c, c->allowed_uid, req, resp, sizeof(resp));
	explicit_bzero(req, sizeof(req));
	wipe_picker(p);

	if (strncmp(resp, "OK ", 3) == 0) {
		p->status[0] = '\0';
	} else {
		snprintf(p->status, sizeof(p->status), "Selection failed");
	}
	explicit_bzero(resp, sizeof(resp));
	if (c->state == SEAT_STATE_LOCKED)
		vt_prompt_render(p);
}

static void cancel_pick(struct vt_prompt *p, struct seat_controller *c) {
	char resp[128];
	(void)controller_dispatch_auth(c, c->allowed_uid, "CANCEL\n", resp,
								   sizeof(resp));
	explicit_bzero(resp, sizeof(resp));
	wipe_picker(p);
	p->status[0] = '\0';
	vt_prompt_render(p);
}

int vt_prompt_handle_input(struct vt_prompt *p, struct seat_controller *c) {
	if (!p || !p->active || !c || p->fd < 0)
		return -EINVAL;

	unsigned char buf[64];
	ssize_t n = read(p->fd, buf, sizeof(buf));
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;
		return -errno;
	}
	if (n == 0)
		return 0;

	int64_t now_ms = tam_monotonic_ms();
	bool dirty = false;

	for (ssize_t i = 0; i < n; i++) {
		unsigned char ch = buf[i];

		if (p->mode == VT_PROMPT_PICKER) {
			if (ch == 0x1B) {
				if (i + 2 < n && buf[i + 1] == '[') {
					unsigned char code = buf[i + 2];
					i += 2;
					if (code == 'A' && p->pick_selected > 0) {
						p->pick_selected--;
						dirty = true;
					} else if (code == 'B' &&
							   p->pick_selected + 1 < p->pick_count) {
						p->pick_selected++;
						dirty = true;
					}
					continue;
				}
				cancel_pick(p, c);
				dirty = false;
				continue;
			}
			if (ch == 'k' && p->pick_selected > 0) {
				p->pick_selected--;
				dirty = true;
			} else if (ch == 'j' && p->pick_selected + 1 < p->pick_count) {
				p->pick_selected++;
				dirty = true;
			} else if (ch >= '1' && ch <= '9') {
				size_t idx = (size_t)(ch - '1');
				if (idx < p->pick_count) {
					p->pick_selected = idx;
					dirty = true;
				}
			} else if (ch == '\r' || ch == '\n') {
				submit_pick(p, c);
				dirty = false;
				if (c->state != SEAT_STATE_LOCKED)
					break;
			}
			continue;
		}

		/* Idle passcode entry mode. */
		if (ch == 0x1B) {
			/* Ignore CSI escape sequences; bare Esc clears buffer. */
			if (i + 1 < n && buf[i + 1] == '[') {
				i += 1;
				while (i + 1 < n &&
					   !(buf[i + 1] >= '@' && buf[i + 1] <= '~'))
					i++;
				if (i + 1 < n)
					i++;
				continue;
			}
			if (p->pass_len > 0 || p->status[0] != '\0') {
				wipe_pass(p);
				p->status[0] = '\0';
				p->last_input_ms = 0;
				dirty = true;
			}
			continue;
		}
		if (ch == 0x15) { /* Ctrl+U */
			if (p->pass_len > 0) {
				wipe_pass(p);
				p->last_input_ms = 0;
				dirty = true;
			}
			continue;
		}
		if (ch == 0x7F || ch == 0x08) { /* DEL / Backspace */
			if (p->pass_len > 0) {
				pop_utf8_char(p);
				p->last_input_ms = now_ms;
				dirty = true;
			}
			continue;
		}
		if (ch == '\r' || ch == '\n') {
			submit_passcode(p, c);
			dirty = false;
			if (c->state != SEAT_STATE_LOCKED)
				break;
			continue;
		}
		if (ch >= 0x20 && ch != 0x7F) {
			if (p->pass_len < TAM_HIDE_VT_PROMPT_MAX_PASS) {
				p->pass[p->pass_len++] = (char)ch;
				p->pass[p->pass_len] = '\0';
				p->status[0] = '\0';
				p->last_input_ms = now_ms;
				dirty = true;
			}
		}
	}

	explicit_bzero(buf, sizeof(buf));
	if (dirty && c->state == SEAT_STATE_LOCKED)
		vt_prompt_render(p);
	return 0;
}

void vt_prompt_destroy(struct vt_prompt *p) {
	if (!p)
		return;
	wipe_pass(p);
	wipe_picker(p);
	if (p->fd >= 0) {
		if (p->termios_saved)
			(void)tcsetattr(p->fd, TCSANOW, &p->saved_termios);
		if (p->owns_fd)
			close(p->fd);
		p->fd = -1;
	}
	p->active = false;
	p->termios_saved = false;
}
