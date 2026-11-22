/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_AUTH_H
#define TAM_HIDE_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum auth_outcome {
	AUTH_OK = 0,
	AUTH_DENIED = 1,
	AUTH_ERROR = 2,
};

typedef enum auth_outcome (*auth_verify_fn)(const char *password, size_t len,
											void *ctx);

struct auth_job {
	pid_t pid;
	int result_fd;
};

/* Initialize an idle auth_job handle. */
void auth_job_init(struct auth_job *job);

/*
 * Start an asynchronous authentication check in a forked child process.
 * Writes password over an internal pipe (never argv/env) and immediately
 * wipes the caller's password buffer via explicit_bzero().
 * Returns 0 on success (job->result_fd is pollable), negative errno on error.
 */
int auth_start(struct auth_job *job, auth_verify_fn verify_fn, void *ctx,
			   char *password, size_t len);

/*
 * Complete a pending auth_job once job->result_fd is readable/closed.
 * Always fails closed (AUTH_ERROR) if the child crashes or exits abnormally.
 */
enum auth_outcome auth_finish(struct auth_job *job);

/* Abort any in-flight auth_job (e.g. during shutdown). */
void auth_cancel(struct auth_job *job);

/* Phase 1 PAM backend: authenticates the calling user via PAM. */
enum auth_outcome auth_pam_verify(const char *password, size_t len, void *ctx);

#endif
