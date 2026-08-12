/*
 * vekd_session.h - Session management for the vekd web dashboard.
 *
 * Handles session token generation, cookie-based authentication,
 * and session validation.
 */
#ifndef VEKD_SESSION_H
#define VEKD_SESSION_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VEKD_SESSION_TOKEN_LEN 32
#define VEKD_SESSION_TOKEN_HEX_LEN 64
#define VEKD_MAX_SESSIONS 64
#define VEKD_SESSION_TIMEOUT_SEC (3600 * 24) /* 24 hours */

typedef struct {
    char token[VEKD_SESSION_TOKEN_HEX_LEN + 1];
    int64_t user_id;
    int64_t created_at;
    int64_t last_active;
} VekdSession;

typedef struct {
    VekdSession sessions[VEKD_MAX_SESSIONS];
    int count;
} VekdSessionStore;

/* Initialize the session store */
void vekd_session_init(VekdSessionStore *store);

/* Create a new session for the given user. Returns the token, or NULL on error. */
const char *vekd_session_create(VekdSessionStore *store, int64_t user_id);

/* Validate a session token. Returns the session, or NULL if invalid/expired. */
VekdSession *vekd_session_validate(VekdSessionStore *store, const char *token);

/* Destroy a session (logout) */
void vekd_session_destroy(VekdSessionStore *store, const char *token);

/* Extract session token from a Cookie header value.
 * Looks for "session=<token>" in the cookie string.
 * Returns true if found and copies token to out_token. */
bool vekd_session_extract_cookie(const char *cookie_header, uint32_t cookie_len,
                                 char *out_token, size_t out_len);

/* Generate a Set-Cookie header value for a session token */
int vekd_session_format_cookie(const char *token, char *buf, size_t buflen);

#endif /* VEKD_SESSION_H */
