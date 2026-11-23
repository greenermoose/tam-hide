/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_AUTH_H
#define TAM_HIDE_AUTH_H

#include "lock_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum auth_outcome {
	AUTH_OK = 0,
	AUTH_DENIED = 1,
	AUTH_ERROR = 2,
	AUTH_PICK = 3,
	AUTH_LOCKOUT = 4,
};

struct auth_result {
	enum auth_outcome outcome;
	int lockout_sec;
	char pick_token[TAM_HIDE_PICK_TOKEN_MAX];
	size_t pick_count;
	struct lock_pick_item pick_items[TAM_HIDE_MAX_PICK_ITEMS];
};

typedef enum auth_outcome (*auth_verify_fn)(const char *password, size_t len,
											void *ctx);

typedef void (*auth_verify_ex_fn)(const char *password, size_t len, void *ctx,
								  struct auth_result *out);

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

/* Extended version supporting AUTH_PICK (candidate list) and AUTH_LOCKOUT. */
int auth_start_ex(struct auth_job *job, auth_verify_ex_fn verify_fn, void *ctx,
				  char *password, size_t len);

/*
 * Complete a pending auth_job once job->result_fd is readable/closed.
 * Always fails closed (AUTH_ERROR) if the child crashes or exits abnormally.
 */
enum auth_outcome auth_finish(struct auth_job *job);

/* Extended version returning full auth_result (including picker items). */
void auth_finish_ex(struct auth_job *job, struct auth_result *out);

/* Abort any in-flight auth_job (e.g. during shutdown). */
void auth_cancel(struct auth_job *job);

/* Phase 1 PAM backend: authenticates the calling user via PAM. */
enum auth_outcome auth_pam_verify(const char *password, size_t len, void *ctx);

#endif
