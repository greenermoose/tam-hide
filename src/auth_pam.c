/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth.h"

#include <pwd.h>
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct pam_conv_ctx {
	const char *password;
	size_t len;
};

static int pam_conv_cb(int num_msg, const struct pam_message **msg,
					   struct pam_response **resp, void *appdata_ptr) {
	if (num_msg <= 0 || !msg || !resp || !appdata_ptr)
		return PAM_CONV_ERR;

	struct pam_conv_ctx *pctx = appdata_ptr;
	struct pam_response *replies = calloc((size_t)num_msg, sizeof(*replies));
	if (!replies)
		return PAM_BUF_ERR;

	for (int i = 0; i < num_msg; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_OFF:
			replies[i].resp = strndup(pctx->password, pctx->len);
			if (!replies[i].resp) {
				for (int j = 0; j < i; j++) {
					if (replies[j].resp) {
						explicit_bzero(replies[j].resp, strlen(replies[j].resp));
						free(replies[j].resp);
					}
				}
				free(replies);
				return PAM_BUF_ERR;
			}
			break;
		case PAM_PROMPT_ECHO_ON:
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			replies[i].resp = NULL;
			break;
		default:
			for (int j = 0; j < i; j++) {
				if (replies[j].resp) {
					explicit_bzero(replies[j].resp, strlen(replies[j].resp));
					free(replies[j].resp);
				}
			}
			free(replies);
			return PAM_CONV_ERR;
		}
	}

	*resp = replies;
	return PAM_SUCCESS;
}

static const char *select_pam_service(void) {
	if (access("/etc/pam.d/tam-hide", R_OK) == 0)
		return "tam-hide";
	return "system-auth";
}

enum auth_outcome auth_pam_verify(const char *password, size_t len, void *ctx) {
	(void)ctx;
	if (!password || len == 0)
		return AUTH_DENIED;

	struct passwd *pw = getpwuid(getuid());
	if (!pw || !pw->pw_name || !*pw->pw_name)
		return AUTH_ERROR;

	struct pam_conv_ctx pctx = {
		.password = password,
		.len = len,
	};
	struct pam_conv conv = {
		.conv = pam_conv_cb,
		.appdata_ptr = &pctx,
	};

	pam_handle_t *pamh = NULL;
	int rc = pam_start(select_pam_service(), pw->pw_name, &conv, &pamh);
	if (rc != PAM_SUCCESS || !pamh)
		return AUTH_ERROR;

	rc = pam_authenticate(pamh, PAM_SILENT);
	if (rc == PAM_SUCCESS) {
		int acct_rc = pam_acct_mgmt(pamh, PAM_SILENT);
		if (acct_rc != PAM_SUCCESS && acct_rc != PAM_NEW_AUTHTOK_REQD)
			rc = acct_rc;
	}

	int end_rc = pam_end(pamh, rc);
	(void)end_rc;

	if (rc == PAM_SUCCESS)
		return AUTH_OK;
	if (rc == PAM_AUTH_ERR || rc == PAM_MAXTRIES || rc == PAM_PERM_DENIED ||
		rc == PAM_USER_UNKNOWN || rc == PAM_ACCT_EXPIRED)
		return AUTH_DENIED;
	return AUTH_ERROR;
}
