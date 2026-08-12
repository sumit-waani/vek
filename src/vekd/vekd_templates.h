/*
 * vekd_templates.h - Server-side HTML rendering for the vekd web dashboard.
 *
 * All HTML is rendered as C string literals with htmx attributes for
 * interactivity. No client-side framework or build step required.
 */
#ifndef VEKD_TEMPLATES_H
#define VEKD_TEMPLATES_H

#include "vekd_db.h"
#include <stddef.h>

/* Maximum rendered page size */
#define VEKD_PAGE_MAX_SIZE (64 * 1024)

/* Dynamic string buffer for building HTML */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} VekdBuf;

/* Initialize a buffer */
void vekd_buf_init(VekdBuf *buf, size_t initial_cap);

/* Append raw string to buffer */
void vekd_buf_append(VekdBuf *buf, const char *str);

/* Append with format */
void vekd_buf_printf(VekdBuf *buf, const char *fmt, ...);

/* Append HTML-escaped string */
void vekd_buf_append_escaped(VekdBuf *buf, const char *str);

/* Free buffer */
void vekd_buf_free(VekdBuf *buf);

/* Page rendering functions - each returns a malloc'd string (caller frees) */

/* Render the full HTML layout wrapper around inner content */
void vekd_tpl_layout(VekdBuf *buf, const char *title, const char *content,
                     bool logged_in);

/* Render the login page */
char *vekd_tpl_login(const char *error_msg);

/* Render the dashboard page */
char *vekd_tpl_dashboard(VekdDB *db);

/* Render the new app form */
char *vekd_tpl_app_new(const char *error_msg);

/* Render app detail page */
char *vekd_tpl_app_detail(VekdDB *db, VekdApp *app);

/* Render app list partial (for htmx swap) */
char *vekd_tpl_app_list_partial(VekdDB *db);

/* Render settings/users page */
char *vekd_tpl_settings_users(VekdDB *db);

/* Render settings/cloudflare page */
char *vekd_tpl_settings_cloudflare(VekdDB *db);

/* Render settings/backup page */
char *vekd_tpl_settings_backup(void);

/* Render logs partial (for htmx polling) */
char *vekd_tpl_logs_partial(VekdDB *db, int64_t app_id);

/* Render env vars partial */
char *vekd_tpl_env_partial(VekdDB *db, int64_t app_id);

#endif /* VEKD_TEMPLATES_H */
