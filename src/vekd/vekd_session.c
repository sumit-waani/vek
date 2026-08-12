/*
 * vekd_session.c - Session management implementation.
 */
#include "vekd_session.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

void vekd_session_init(VekdSessionStore *store) {
    memset(store, 0, sizeof(VekdSessionStore));
}

static bool generate_token(char *out, size_t out_len) {
    if (out_len < VEKD_SESSION_TOKEN_HEX_LEN + 1) return false;

    uint8_t bytes[VEKD_SESSION_TOKEN_LEN];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;

    ssize_t n = read(fd, bytes, sizeof(bytes));
    close(fd);
    if (n != (ssize_t)sizeof(bytes)) return false;

    for (int i = 0; i < VEKD_SESSION_TOKEN_LEN; i++) {
        snprintf(out + (i * 2), 3, "%02x", bytes[i]);
    }
    out[VEKD_SESSION_TOKEN_HEX_LEN] = '\0';
    return true;
}

const char *vekd_session_create(VekdSessionStore *store, int64_t user_id) {
    /* Find a free slot or evict the oldest */
    int slot = -1;
    int64_t oldest_time = INT64_MAX;
    int oldest_slot = 0;

    for (int i = 0; i < VEKD_MAX_SESSIONS; i++) {
        if (store->sessions[i].token[0] == '\0') {
            slot = i;
            break;
        }
        if (store->sessions[i].last_active < oldest_time) {
            oldest_time = store->sessions[i].last_active;
            oldest_slot = i;
        }
    }

    if (slot < 0) {
        /* Evict oldest session */
        slot = oldest_slot;
    }

    VekdSession *sess = &store->sessions[slot];
    if (!generate_token(sess->token, sizeof(sess->token))) {
        return NULL;
    }

    sess->user_id = user_id;
    sess->created_at = (int64_t)time(NULL);
    sess->last_active = sess->created_at;

    if (slot >= store->count) {
        store->count = slot + 1;
    }

    return sess->token;
}

VekdSession *vekd_session_validate(VekdSessionStore *store, const char *token) {
    if (!token || token[0] == '\0') return NULL;

    int64_t now = (int64_t)time(NULL);

    for (int i = 0; i < VEKD_MAX_SESSIONS; i++) {
        if (store->sessions[i].token[0] == '\0') continue;

        if (strcmp(store->sessions[i].token, token) == 0) {
            /* Check expiry */
            if (now - store->sessions[i].created_at > VEKD_SESSION_TIMEOUT_SEC) {
                /* Expired - clear it */
                memset(&store->sessions[i], 0, sizeof(VekdSession));
                return NULL;
            }
            store->sessions[i].last_active = now;
            return &store->sessions[i];
        }
    }
    return NULL;
}

void vekd_session_destroy(VekdSessionStore *store, const char *token) {
    if (!token) return;

    for (int i = 0; i < VEKD_MAX_SESSIONS; i++) {
        if (strcmp(store->sessions[i].token, token) == 0) {
            memset(&store->sessions[i], 0, sizeof(VekdSession));
            return;
        }
    }
}

bool vekd_session_extract_cookie(const char *cookie_header, uint32_t cookie_len,
                                 char *out_token, size_t out_len) {
    if (!cookie_header || cookie_len == 0) return false;

    /* Look for "session=" in the cookie string */
    const char *needle = "session=";
    size_t needle_len = 8;

    const char *pos = cookie_header;
    const char *end = cookie_header + cookie_len;

    while (pos < end) {
        /* Skip whitespace and separators */
        while (pos < end && (*pos == ' ' || *pos == ';')) pos++;
        if (pos >= end) break;

        if ((size_t)(end - pos) >= needle_len &&
            strncmp(pos, needle, needle_len) == 0) {
            pos += needle_len;
            /* Extract token value until ';' or end */
            const char *val_start = pos;
            while (pos < end && *pos != ';' && *pos != ' ') pos++;
            size_t val_len = (size_t)(pos - val_start);
            if (val_len >= out_len) val_len = out_len - 1;
            memcpy(out_token, val_start, val_len);
            out_token[val_len] = '\0';
            return true;
        }

        /* Skip to next cookie */
        while (pos < end && *pos != ';') pos++;
    }

    return false;
}

int vekd_session_format_cookie(const char *token, char *buf, size_t buflen) {
    return snprintf(buf, buflen,
                    "session=%s; Path=/; HttpOnly; SameSite=Strict",
                    token);
}
