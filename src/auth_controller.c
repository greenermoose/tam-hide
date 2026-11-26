/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth_controller.h"

#include <tam.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int ipc_roundtrip(const char *socket_path, const char *req,
						 size_t req_len, char *resp, size_t resp_size) {
	if (!socket_path || !req || req_len == 0 || !resp || resp_size == 0)
		return -EINVAL;

	int fd = tam_sock_connect(socket_path, 5000);
	if (fd < 0)
		return -errno;

	struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	size_t off = 0;
	while (off < req_len) {
		ssize_t n = write(fd, req + off, req_len - off);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			int err = n < 0 ? -errno : -EIO;
			close(fd);
			return err;
		}
		off += (size_t)n;
	}

	size_t total = 0;
	while (total + 1 < resp_size) {
		ssize_t n = read(fd, resp + total, resp_size - 1 - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			int err = -errno;
			close(fd);
			return err;
		}
		if (n == 0)
			break;
		total += (size_t)n;
	}
	close(fd);
	resp[total] = '\0';
	return (total > 0) ? 0 : -EPIPE;
}

int auth_controller_confirm_lock(const char *socket_path, int vt_nr) {
	if (!socket_path || vt_nr <= 0)
		return -EINVAL;
	char req[64];
	int n = snprintf(req, sizeof(req), "CONFIRM_LOCK %d\n", vt_nr);
	if (n < 0 || (size_t)n >= sizeof(req))
		return -EINVAL;
	char resp[256];
	int rc = ipc_roundtrip(socket_path, req, (size_t)n, resp, sizeof(resp));
	if (rc < 0)
		return rc;
	return (strncmp(resp, "OK ", 3) == 0) ? 0 : -EACCES;
}

void auth_controller_on_locked(void *vctx) {
	struct auth_controller_ctx *ctx = vctx;
	if (!ctx || !ctx->socket_path || ctx->session_vt <= 0)
		return;
	(void)auth_controller_confirm_lock(ctx->socket_path, ctx->session_vt);
}

void auth_controller_verify(const char *password, size_t len, void *vctx,
							struct auth_result *out) {
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->outcome = AUTH_ERROR;

	struct auth_controller_ctx *ctx = vctx;
	if (!ctx || !ctx->socket_path || !password || len == 0 ||
		len > TAM_HIDE_MAX_PASS_BYTES)
		return;

	char req[TAM_HIDE_MAX_PASS_BYTES + 16];
	memcpy(req, "AUTH ", 5);
	memcpy(req + 5, password, len);
	req[5 + len] = '\n';
	size_t req_len = 5 + len + 1;

	char resp[4096];
	int rc = ipc_roundtrip(ctx->socket_path, req, req_len, resp, sizeof(resp));
	explicit_bzero(req, sizeof(req));
	if (rc < 0) {
		explicit_bzero(resp, sizeof(resp));
		return;
	}

	if (strncmp(resp, "OK ", 3) == 0) {
		out->outcome = AUTH_OK;
	} else if (strncmp(resp, "ERR lockout sec=", 16) == 0) {
		out->outcome = AUTH_LOCKOUT;
		out->lockout_sec = atoi(resp + 16);
		if (out->lockout_sec < 1)
			out->lockout_sec = 1;
	} else if (strncmp(resp, "ERR denied", 10) == 0) {
		out->outcome = AUTH_DENIED;
	} else if (strncmp(resp, "PICK ", 5) == 0) {
		char *saveptr = NULL;
		char *line = strtok_r(resp, "\n", &saveptr);
		char tok[TAM_HIDE_PICK_TOKEN_MAX];
		unsigned long expected_count = 0;
		if (line &&
			sscanf(line, "PICK token=%39s count=%lu", tok, &expected_count) == 2) {
			snprintf(out->pick_token, sizeof(out->pick_token), "%s", tok);
			size_t parsed = 0;
			while ((line = strtok_r(NULL, "\n", &saveptr)) != NULL) {
				if (strcmp(line, "END") == 0)
					break;
				if (parsed >= TAM_HIDE_MAX_PICK_ITEMS)
					continue;
				int idx = 0, vt = 0;
				unsigned uid = 0;
				char user[64], kind[16];
				if (sscanf(line, "ITEM %d user=%63s uid=%u vt=%d kind=%15s",
						   &idx, user, &uid, &vt, kind) == 5) {
					struct lock_pick_item *it = &out->pick_items[parsed++];
					it->index = idx;
					it->uid = (uid_t)uid;
					it->vt_nr = vt;
					snprintf(it->username, sizeof(it->username), "%s", user);
					snprintf(it->kind, sizeof(it->kind), "%s", kind);
				}
			}
			if (parsed > 0) {
				out->pick_count = parsed;
				out->outcome = AUTH_PICK;
			}
		}
	}

	explicit_bzero(resp, sizeof(resp));
}

enum auth_outcome auth_controller_select(const char *token, int index,
										 void *vctx) {
	struct auth_controller_ctx *ctx = vctx;
	if (!ctx || !ctx->socket_path || !token || token[0] == '\0' || index < 0)
		return AUTH_ERROR;

	char req[128];
	int n = snprintf(req, sizeof(req), "SELECT %s %d\n", token, index);
	if (n < 0 || (size_t)n >= sizeof(req))
		return AUTH_ERROR;

	char resp[256];
	int rc = ipc_roundtrip(ctx->socket_path, req, (size_t)n, resp, sizeof(resp));
	explicit_bzero(req, sizeof(req));
	if (rc < 0) {
		explicit_bzero(resp, sizeof(resp));
		return AUTH_ERROR;
	}
	enum auth_outcome res =
		(strncmp(resp, "OK ", 3) == 0) ? AUTH_OK : AUTH_DENIED;
	explicit_bzero(resp, sizeof(resp));
	return res;
}

void auth_controller_cancel(void *vctx) {
	struct auth_controller_ctx *ctx = vctx;
	if (!ctx || !ctx->socket_path)
		return;
	const char req[] = "CANCEL\n";
	char resp[128];
	(void)ipc_roundtrip(ctx->socket_path, req, sizeof(req) - 1, resp,
						sizeof(resp));
	explicit_bzero(resp, sizeof(resp));
}
