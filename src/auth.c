/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth.h"
#include "lock_state.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_all(int fd, const void *buf, size_t len) {
	const uint8_t *p = buf;
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		off += (size_t)n;
	}
	return 0;
}

static int read_all(int fd, void *buf, size_t len) {
	uint8_t *p = buf;
	size_t off = 0;
	while (off < len) {
		ssize_t n = read(fd, p + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -EPIPE;
		off += (size_t)n;
	}
	return 0;
}

void auth_job_init(struct auth_job *job) {
	if (!job)
		return;
	job->pid = -1;
	job->result_fd = -1;
}

struct simple_wrap {
	auth_verify_fn fn;
	void *ctx;
};

static void simple_verify_adapter(const char *password, size_t len, void *vctx,
								  struct auth_result *out) {
	struct simple_wrap *w = vctx;
	out->outcome = w->fn(password, len, w->ctx);
}

int auth_start_ex(struct auth_job *job, auth_verify_ex_fn verify_fn, void *ctx,
				  char *password, size_t len) {
	if (!job || !verify_fn || !password || len == 0 ||
		len > TAM_HIDE_MAX_PASS_BYTES) {
		if (password && len > 0)
			explicit_bzero(password, len);
		return -EINVAL;
	}
	if (job->pid > 0 || job->result_fd >= 0) {
		explicit_bzero(password, len);
		return -EBUSY;
	}

	int in_pipe[2] = {-1, -1};
	int out_pipe[2] = {-1, -1};
	if (pipe2(in_pipe, O_CLOEXEC) < 0) {
		int err = -errno;
		explicit_bzero(password, len);
		return err;
	}
	if (pipe2(out_pipe, O_CLOEXEC) < 0) {
		int err = -errno;
		close(in_pipe[0]);
		close(in_pipe[1]);
		explicit_bzero(password, len);
		return err;
	}

	pid_t pid = fork();
	if (pid < 0) {
		int err = -errno;
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		explicit_bzero(password, len);
		return err;
	}

	if (pid == 0) {
		/* Child worker: wipe parent's copy immediately, read from pipe. */
		explicit_bzero(password, len);
		close(in_pipe[1]);
		close(out_pipe[0]);
		(void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

		struct auth_result res;
		memset(&res, 0, sizeof(res));
		res.outcome = AUTH_ERROR;

		uint32_t plen = 0;
		char child_buf[TAM_HIDE_MAX_PASS_BYTES + 1];
		memset(child_buf, 0, sizeof(child_buf));

		if (read_all(in_pipe[0], &plen, sizeof(plen)) == 0 &&
			plen > 0 && plen <= TAM_HIDE_MAX_PASS_BYTES &&
			read_all(in_pipe[0], child_buf, plen) == 0) {
			child_buf[plen] = '\0';
			close(in_pipe[0]);
			verify_fn(child_buf, plen, ctx, &res);
		} else {
			close(in_pipe[0]);
		}

		explicit_bzero(child_buf, sizeof(child_buf));
		(void)write_all(out_pipe[1], &res, sizeof(res));
		explicit_bzero(&res, sizeof(res));
		close(out_pipe[1]);
		_exit(0);
	}

	/* Parent: send length + password, wipe immediately. */
	close(in_pipe[0]);
	close(out_pipe[1]);

	uint32_t plen = (uint32_t)len;
	int wrc = write_all(in_pipe[1], &plen, sizeof(plen));
	if (wrc == 0)
		wrc = write_all(in_pipe[1], password, len);
	explicit_bzero(password, len);
	close(in_pipe[1]);

	if (wrc < 0) {
		close(out_pipe[0]);
		(void)kill(pid, SIGKILL);
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
		return wrc;
	}

	job->pid = pid;
	job->result_fd = out_pipe[0];
	return 0;
}

int auth_start(struct auth_job *job, auth_verify_fn verify_fn, void *ctx,
			   char *password, size_t len) {
	if (!verify_fn) {
		if (password && len > 0)
			explicit_bzero(password, len);
		return -EINVAL;
	}
	struct simple_wrap wrap = { .fn = verify_fn, .ctx = ctx };
	return auth_start_ex(job, simple_verify_adapter, &wrap, password, len);
}

void auth_finish_ex(struct auth_job *job, struct auth_result *out) {
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	out->outcome = AUTH_ERROR;

	if (!job || job->pid <= 0 || job->result_fd < 0)
		return;

	struct auth_result tmp;
	memset(&tmp, 0, sizeof(tmp));
	int rrc = read_all(job->result_fd, &tmp, sizeof(tmp));
	close(job->result_fd);
	job->result_fd = -1;

	int status = 0;
	while (waitpid(job->pid, &status, 0) < 0 && errno == EINTR)
		;
	job->pid = -1;

	if (rrc < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return;

	if (tmp.outcome == AUTH_OK || tmp.outcome == AUTH_DENIED ||
		tmp.outcome == AUTH_PICK || tmp.outcome == AUTH_LOCKOUT) {
		if (tmp.pick_count > TAM_HIDE_MAX_PICK_ITEMS)
			tmp.pick_count = TAM_HIDE_MAX_PICK_ITEMS;
		*out = tmp;
	}
	explicit_bzero(&tmp, sizeof(tmp));
}

enum auth_outcome auth_finish(struct auth_job *job) {
	struct auth_result res;
	auth_finish_ex(job, &res);
	enum auth_outcome outcome = res.outcome;
	explicit_bzero(&res, sizeof(res));
	return outcome;
}

void auth_cancel(struct auth_job *job) {
	if (!job)
		return;
	if (job->result_fd >= 0) {
		close(job->result_fd);
		job->result_fd = -1;
	}
	if (job->pid > 0) {
		(void)kill(job->pid, SIGTERM);
		while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR)
			;
		job->pid = -1;
	}
}
