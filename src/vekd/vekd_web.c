/*
 * vekd_web.c - Web UI route handlers for the vekd dashboard.
 *
 * All routes serve HTML pages with htmx for interactivity.
 * Authentication is cookie-based using session tokens.
 */
#define _GNU_SOURCE
#include "vekd_web.h"
#include "vekd_templates.h"
#include "vekd_config.h"
#include "vekd_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Route handler IDs (stored as void* in the router) */
#define ROUTE_LOGIN_GET      ((void*)1)
#define ROUTE_LOGIN_POST     ((void*)2)
#define ROUTE_LOGOUT         ((void*)3)
#define ROUTE_DASHBOARD      ((void*)4)
#define ROUTE_APPS_NEW       ((void*)5)
#define ROUTE_APPS_CREATE    ((void*)6)
#define ROUTE_APPS_DETAIL    ((void*)7)
#define ROUTE_APPS_DEPLOY    ((void*)8)
#define ROUTE_APPS_START     ((void*)9)
#define ROUTE_APPS_STOP      ((void*)10)
#define ROUTE_APPS_RESTART   ((void*)11)
#define ROUTE_APPS_DELETE    ((void*)12)
#define ROUTE_APPS_ENV_GET   ((void*)13)
#define ROUTE_APPS_ENV_SET   ((void*)14)
#define ROUTE_APPS_ENV_DEL   ((void*)15)
#define ROUTE_APPS_LOGS      ((void*)16)
#define ROUTE_APPS_LIST      ((void*)17)
#define ROUTE_SETTINGS_USERS_GET  ((void*)18)
#define ROUTE_SETTINGS_USERS_POST ((void*)19)
#define ROUTE_SETTINGS_CF_GET     ((void*)20)
#define ROUTE_SETTINGS_CF_POST    ((void*)21)
#define ROUTE_SETTINGS_BACKUP_GET ((void*)22)
#define ROUTE_SETTINGS_BACKUP_POST ((void*)23)
#define ROUTE_ROOT           ((void*)24)

/* ---- Helper: URL decode ---- */

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t url_decode(const char *src, size_t src_len, char *dst, size_t dst_len) {
    size_t si = 0, di = 0;
    while (si < src_len && di < dst_len - 1) {
        if (src[si] == '%' && si + 2 < src_len) {
            int h = hex_digit(src[si + 1]);
            int l = hex_digit(src[si + 2]);
            if (h >= 0 && l >= 0) {
                dst[di++] = (char)(h * 16 + l);
                si += 3;
                continue;
            }
        }
        if (src[si] == '+') {
            dst[di++] = ' ';
            si++;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return di;
}

/* ---- Helper: Parse form body (application/x-www-form-urlencoded) ---- */

typedef struct {
    char key[256];
    char value[1024];
} FormField;

typedef struct {
    FormField fields[16];
    int count;
} FormData;

static void parse_form_body(const char *body, size_t body_len, FormData *form) {
    form->count = 0;
    if (!body || body_len == 0) return;

    const char *pos = body;
    const char *end = body + body_len;

    while (pos < end && form->count < 16) {
        /* Find key */
        const char *eq = pos;
        while (eq < end && *eq != '=' && *eq != '&') eq++;
        if (eq == pos || eq >= end || *eq != '=') {
            /* Skip to next & */
            while (pos < end && *pos != '&') pos++;
            if (pos < end) pos++;
            continue;
        }

        size_t key_len = (size_t)(eq - pos);
        url_decode(pos, key_len, form->fields[form->count].key,
                   sizeof(form->fields[form->count].key));

        /* Find value */
        const char *val_start = eq + 1;
        const char *val_end = val_start;
        while (val_end < end && *val_end != '&') val_end++;

        size_t val_len = (size_t)(val_end - val_start);
        url_decode(val_start, val_len, form->fields[form->count].value,
                   sizeof(form->fields[form->count].value));

        form->count++;
        pos = val_end;
        if (pos < end) pos++; /* skip '&' */
    }
}

static const char *form_get(FormData *form, const char *key) {
    for (int i = 0; i < form->count; i++) {
        if (strcmp(form->fields[i].key, key) == 0) {
            return form->fields[i].value;
        }
    }
    return NULL;
}

/* ---- Helper: Simple password hashing using PBKDF2-SHA256 ---- */

/* (Moved to vekd_crypto.c: vekd_crypto_hash_password / vekd_crypto_verify_password) */

/* ---- Helper: Check authentication ---- */

static VekdSession *check_auth(VekdWebContext *ctx, HttpRequest *req) {
    uint32_t cookie_len = 0;
    const char *cookie = http_get_header(req, "Cookie", &cookie_len);
    if (!cookie) return NULL;

    char token[VEKD_SESSION_TOKEN_HEX_LEN + 1];
    if (!vekd_session_extract_cookie(cookie, cookie_len, token, sizeof(token))) {
        return NULL;
    }

    return vekd_session_validate(&ctx->sessions, token);
}

/* ---- Helper: Send redirect ---- */

static void send_redirect(HttpServer *server, Connection *conn,
                          const char *location, const char *set_cookie) {
    char hdr[1024];
    int hdr_len;
    if (set_cookie) {
        hdr_len = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 302 Found\r\n"
            "Location: %s\r\n"
            "Set-Cookie: %s\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n", location, set_cookie);
    } else {
        hdr_len = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 302 Found\r\n"
            "Location: %s\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n", location);
    }
    event_loop_conn_write(&server->loop, conn, hdr, (size_t)hdr_len);
}

/* ---- Helper: Send HTML response ---- */

static void send_html(HttpServer *server, Connection *conn, char *html) {
    if (!html) {
        http_response_send(server, conn, 500, NULL, "text/plain",
                           "Internal Error", 14, false);
        return;
    }
    http_response_send(server, conn, 200, "OK", "text/html",
                       html, strlen(html), false);
    free(html);
}

/* ---- Helper: Send short HTML partial ---- */

static void send_partial(HttpServer *server, Connection *conn, char *html) {
    if (!html) {
        http_response_send(server, conn, 500, NULL, "text/plain",
                           "Internal Error", 14, false);
        return;
    }
    http_response_send(server, conn, 200, "OK", "text/html",
                       html, strlen(html), false);
    free(html);
}

/* ---- Helper: Get app by id from route param ---- */

static int get_app_by_id_param(VekdWebContext *ctx, RouteParams *params, VekdApp *app) {
    /* Find the "id" param */
    const char *id_str = NULL;
    uint32_t id_len = 0;
    for (int i = 0; i < params->count; i++) {
        if (params->params[i].name_len == 2 &&
            strncmp(params->params[i].name, "id", 2) == 0) {
            id_str = params->params[i].value;
            id_len = params->params[i].value_len;
            break;
        }
    }
    if (!id_str) return -1;

    /* Convert to int64 */
    char id_buf[32];
    if (id_len >= sizeof(id_buf)) return -1;
    memcpy(id_buf, id_str, id_len);
    id_buf[id_len] = '\0';
    int64_t app_id = atoll(id_buf);
    if (app_id <= 0) return -1;

    /* Look up by ID */
    const char *sql = "SELECT id, name, repo_url, branch, domain, port, "
                      "cpu_weight, memory_max, state, current_release, "
                      "created_at, updated_at FROM apps WHERE id = ?";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(ctx->db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, app_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }

    memset(app, 0, sizeof(VekdApp));
    app->id = sqlite3_column_int64(stmt, 0);
    const char *col;
    col = (const char *)sqlite3_column_text(stmt, 1);
    if (col) strncpy(app->name, col, sizeof(app->name) - 1);
    col = (const char *)sqlite3_column_text(stmt, 2);
    if (col) strncpy(app->repo_url, col, sizeof(app->repo_url) - 1);
    col = (const char *)sqlite3_column_text(stmt, 3);
    if (col) strncpy(app->branch, col, sizeof(app->branch) - 1);
    col = (const char *)sqlite3_column_text(stmt, 4);
    if (col) strncpy(app->domain, col, sizeof(app->domain) - 1);
    app->port = sqlite3_column_int(stmt, 5);
    app->cpu_weight = sqlite3_column_int(stmt, 6);
    app->memory_max = sqlite3_column_int64(stmt, 7);
    col = (const char *)sqlite3_column_text(stmt, 8);
    if (col) strncpy(app->state, col, sizeof(app->state) - 1);
    col = (const char *)sqlite3_column_text(stmt, 9);
    if (col) strncpy(app->current_release, col, sizeof(app->current_release) - 1);
    app->created_at = sqlite3_column_int64(stmt, 10);
    app->updated_at = sqlite3_column_int64(stmt, 11);

    sqlite3_finalize(stmt);
    return 0;
}

/* ---- Helper: Get env key from route params ---- */

static const char *get_env_key_param(RouteParams *params, uint32_t *out_len) {
    for (int i = 0; i < params->count; i++) {
        if (params->params[i].name_len == 3 &&
            strncmp(params->params[i].name, "key", 3) == 0) {
            *out_len = params->params[i].value_len;
            return params->params[i].value;
        }
    }
    *out_len = 0;
    return NULL;
}

/* ---- Route: GET / ---- */

static void handle_root(HttpServer *server, Connection *conn,
                        VekdWebContext *ctx, HttpRequest *req,
                        RouteParams *params) {
    (void)params;
    VekdSession *sess = check_auth(ctx, req);
    if (sess) {
        send_redirect(server, conn, "/dashboard", NULL);
    } else {
        send_redirect(server, conn, "/login", NULL);
    }
}

/* ---- Route: GET /login ---- */

static void handle_login_get(HttpServer *server, Connection *conn,
                             VekdWebContext *ctx, HttpRequest *req,
                             RouteParams *params) {
    (void)ctx; (void)req; (void)params;
    char *html = vekd_tpl_login(NULL);
    send_html(server, conn, html);
}

/* ---- Route: POST /login ---- */

static void handle_login_post(HttpServer *server, Connection *conn,
                              VekdWebContext *ctx, HttpRequest *req,
                              RouteParams *params) {
    (void)params;
    FormData form;
    parse_form_body(req->body, req->body_len, &form);

    const char *email = form_get(&form, "email");
    const char *password = form_get(&form, "password");

    if (!email || !password || !email[0] || !password[0]) {
        char *html = vekd_tpl_login("Email and password are required.");
        send_html(server, conn, html);
        return;
    }

    /* Check if any users exist; if not, atomically create the first admin */
    int user_count = vekd_db_user_count(ctx->db);
    if (user_count == 0) {
        char pw_hash[128];
        vekd_crypto_hash_password(password, pw_hash, sizeof(pw_hash));
        vekd_db_user_create_if_no_users(ctx->db, email, pw_hash, 1);
    }

    /* Look up user */
    VekdUser user;
    if (vekd_db_user_get_by_email(ctx->db, email, &user) < 0) {
        char *html = vekd_tpl_login("Invalid email or password.");
        send_html(server, conn, html);
        return;
    }

    /* Verify password */
    if (!vekd_crypto_verify_password(password, user.password_hash)) {
        char *html = vekd_tpl_login("Invalid email or password.");
        send_html(server, conn, html);
        return;
    }

    /* Create session */
    const char *token = vekd_session_create(&ctx->sessions, user.id);
    if (!token) {
        char *html = vekd_tpl_login("Session creation failed.");
        send_html(server, conn, html);
        return;
    }

    char cookie_buf[256];
    vekd_session_format_cookie(token, cookie_buf, sizeof(cookie_buf));
    send_redirect(server, conn, "/dashboard", cookie_buf);
}

/* ---- Route: GET /logout ---- */

static void handle_logout(HttpServer *server, Connection *conn,
                          VekdWebContext *ctx, HttpRequest *req,
                          RouteParams *params) {
    (void)params;
    uint32_t cookie_len = 0;
    const char *cookie = http_get_header(req, "Cookie", &cookie_len);
    if (cookie) {
        char token[VEKD_SESSION_TOKEN_HEX_LEN + 1];
        if (vekd_session_extract_cookie(cookie, cookie_len, token, sizeof(token))) {
            vekd_session_destroy(&ctx->sessions, token);
        }
    }
    send_redirect(server, conn, "/login",
                  "session=; Path=/; HttpOnly; Max-Age=0");
}

/* ---- Route: GET /dashboard ---- */

static void handle_dashboard(HttpServer *server, Connection *conn,
                             VekdWebContext *ctx, HttpRequest *req,
                             RouteParams *params) {
    (void)params;
    if (!check_auth(ctx, req)) {
        send_redirect(server, conn, "/login", NULL);
        return;
    }
    char *html = vekd_tpl_dashboard(ctx->db);
    send_html(server, conn, html);
}

/* ---- Route: GET /apps/list (htmx partial) ---- */

static void handle_apps_list(HttpServer *server, Connection *conn,
                             VekdWebContext *ctx, HttpRequest *req,
                             RouteParams *params) {
    (void)params;
    if (!check_auth(ctx, req)) {
        http_response_send(server, conn, 401, NULL, "text/plain",
                           "Unauthorized", 12, false);
        return;
    }
    char *html = vekd_tpl_app_list_partial(ctx->db);
    send_partial(server, conn, html);
}

/* ---- Route: GET /apps/new ---- */

static void handle_apps_new(HttpServer *server, Connection *conn,
                            VekdWebContext *ctx, HttpRequest *req,
                            RouteParams *params) {
    (void)params;
    if (!check_auth(ctx, req)) {
        send_redirect(server, conn, "/login", NULL);
        return;
    }
    char *html = vekd_tpl_app_new(NULL);
    send_html(server, conn, html);
}

/* ---- Route: POST /apps ---- */

static void handle_apps_create(HttpServer *server, Connection *conn,
                               VekdWebContext *ctx, HttpRequest *req,
                               RouteParams *params) {
    (void)params;
    if (!check_auth(ctx, req)) {
        send_redirect(server, conn, "/login", NULL);
        return;
    }

    FormData form;
    parse_form_body(req->body, req->body_len, &form);

    const char *name = form_get(&form, "name");
    const char *repo_url = form_get(&form, "repo_url");
    const char *branch = form_get(&form, "branch");

    if (!name || !name[0] || !repo_url || !repo_url[0]) {
        char *html = vekd_tpl_app_new("App name and repository URL are required.");
        send_html(server, conn, html);
        return;
    }

    if (!branch || !branch[0]) branch = "main";

    /* Assign a port */
    int port = ctx->next_port++;
    if (port > VEKD_APP_PORT_MAX) {
        char *html = vekd_tpl_app_new("No available ports. Too many apps deployed.");
        send_html(server, conn, html);
        return;
    }

    if (vekd_db_app_create(ctx->db, name, repo_url, branch, port) < 0) {
        char *html = vekd_tpl_app_new("Failed to create app. Name may already exist.");
        send_html(server, conn, html);
        return;
    }

    vekd_db_event_log(ctx->db, 0, "app_created", name);
    send_redirect(server, conn, "/dashboard", NULL);
}

/* ---- Route: GET /apps/:id ---- */

static void handle_apps_detail(HttpServer *server, Connection *conn,
                               VekdWebContext *ctx, HttpRequest *req,
                               RouteParams *params) {
    if (!check_auth(ctx, req)) {
        send_redirect(server, conn, "/login", NULL);
        return;
    }

    VekdApp app;
    if (get_app_by_id_param(ctx, params, &app) < 0) {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "App not found", 13, false);
        return;
    }

    char *html = vekd_tpl_app_detail(ctx->db, &app);
    send_html(server, conn, html);
}

/* ---- Route: POST /apps/:id/deploy ---- */

static void handle_apps_deploy(HttpServer *server, Connection *conn,
                               VekdWebContext *ctx, HttpRequest *req,
                               RouteParams *params) {
    if (!check_auth(ctx, req)) {
        http_response_send(server, conn, 401, NULL, "text/plain",
                           "Unauthorized", 12, false);
        return;
    }

    VekdApp app;
    if (get_app_by_id_param(ctx, params, &app) < 0) {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "App not found", 13, false);
        return;
    }

    /* Log the deploy event */
    vekd_db_event_log(ctx->db, app.id, "deploy", "Deployment triggered via dashboard");
    vekd_db_app_update_state(ctx->db, app.id, "running");

    http_response_send(server, conn, 200, "OK", "text/html",
                       "<span class=\"badge badge-running\">deploying</span>",
                       49, false);
}

/* ---- Route: POST /apps/:id/start ---- */

static void handle_apps_start(HttpServer *server, Connection *conn,
                              VekdWebContext *ctx, HttpRequest *req,
                              RouteParams *params) {
    if (!check_auth(ctx, req)) {
        http_response_send(server, conn, 401, NULL, "text/plain",
                           "Unauthorized", 12, false);
        return;
    }

    VekdApp app;
    if (get_app_by_id_param(ctx, params, &app) < 0) {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "App not found", 13, false);
        return;
    }

    vekd_db_app_update_state(ctx->db, app.id, "running");
    vekd_db_event_log(ctx->db, app.id, "start", "App started via dashboard");

    http_response_send(server, conn, 200, "OK", "text/plain", "OK", 2, false);
}

/* ---- Route: POST /apps/:id/stop ---- */

static void handle_apps_stop(HttpServer *server, Connection *conn,
                             VekdWebContext *ctx, HttpRequest *req,
                             RouteParams *params) {
    if (!check_auth(ctx, req)) {
        http_response_send(server, conn, 401, NULL, "text/plain",
                           "Unauthorized", 12, false);
        return;
    }

    VekdApp app;
    if (get_app_by_id_param(ctx, params, &app) < 0) {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "App not found", 13, false);
        return;
    }

    vekd_db_app_update_state(ctx->db, app.id, "stopped");
    vekd_db_event_log(ctx->db, app.id, "stop", "App stopped via dashboard");

    /* Stop the supervised process if running */
    vekd_supervisor_stop(ctx->supervisor, app.id);

    http_response_send(server, conn, 200, "OK", "text/plain", "OK", 2, false);
}

/* ---- Route: POST /apps/:id/restart ---- */

static void handle_apps_restart(HttpServer *server, Connection *conn,
                                VekdWebContext *ctx, HttpRequest *req,
                                RouteParams *params) {
    if (!check_auth(ctx, req)) {
        http_response_send(server, conn, 401, NULL, "text/plain",
                           "Unauthorized", 12, false);
        return;
    }

    VekdApp app;
    if (get_app_by_id_param(ctx, params, &app) < 0) {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "App not found", 13, false);
        return;
    }

    vekd_db_app_update_state(ctx->db, app.id, "running");
    vekd_db_event_log(ctx->db, app.id, "restart", "App restarted via dashboard");

    http_response_send(server, conn, 200, "OK", "text/plain", "OK", 2, false);
}

/* ---- Route: DELETE /apps/:id ---- */

static void handle_apps_delete(HttpServer *server, Connection *conn,
                               VekdWebContext *ctx, HttpRequest *req,
                               RouteParams *params) {
    if (!check_auth(ctx, req)) {
        http_response_send(server, conn, 401, NULL, "text/plain",
                           "Unauthorized", 12, false);
        return;
    }

    VekdApp app;
    if (get_app_by_id_param(ctx, params, &app) < 0) {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "App not found", 13, false);
        return;
    }

    /* Stop if running */
    vekd_supervisor_stop(ctx->supervisor, app.id);

    /* Delete from DB */
    vekd_db_event_log(ctx->db, app.id, "delete", "App deleted via dashboard");
    vekd_db_app_delete(ctx->db, app.id);

    /* Redirect to dashboard via HX-Redirect header */
    const char *resp =
        "HTTP/1.1 200 OK\r\n"
        "HX-Redirect: /dashboard\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    event_loop_conn_write(&server->loop, conn, resp, strlen(resp));
}

/* ---- Route: GET /apps/:id/env ---- */

static void handle_apps_env_get(HttpServer *server, Connection *conn,
                                VekdWebContext *ctx, HttpRequest *req,
                                RouteParams *params) {
    if (!check_auth(ctx, req)) {
        http_response_send(server, conn, 401, NULL, "text/plain",
                           "Unauthorized", 12, false);
        return;
    }

    VekdApp app;
    if (get_app_by_id_param(ctx, params, &app) < 0) {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "App not found", 13, false);
        return;
    }

    char *html = vekd_tpl_env_partial(ctx->db, app.id);
    send_partial(server, conn, html);
}

/* ---- Route: POST /apps/:id/env ---- */

static void handle_apps_env_set(HttpServer *server, Connection *conn,
                                VekdWebContext *ctx, HttpRequest *req,
                                RouteParams *params) {
    if (!check_auth(ctx, req)) {
        http_response_send(server, conn, 401, NULL, "text/plain",
                           "Unauthorized", 12, false);
        return;
    }

    VekdApp app;
    if (get_app_by_id_param(ctx, params, &app) < 0) {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "App not found", 13, false);
        return;
    }

    FormData form;
    parse_form_body(req->body, req->body_len, &form);

    const char *key = form_get(&form, "key");
    const char *value = form_get(&form, "value");

    if (!key || !key[0]) {
        http_response_send(server, conn, 400, NULL, "text/plain",
                           "Key is required", 15, false);
        return;
    }

    /* Encrypt the value */
    size_t val_len = value ? strlen(value) : 0;
    uint8_t *encrypted = NULL;
    size_t encrypted_len = 0;
    if (val_len > 0) {
        encrypted_len = VEKD_NONCE_SIZE + val_len;
        encrypted = malloc(encrypted_len);
        if (encrypted) {
            vekd_crypto_encrypt(ctx->master_key, (const uint8_t *)value,
                                val_len, encrypted);
        }
    }

    /* Upsert into env_vars */
    const char *sql =
        "INSERT OR REPLACE INTO env_vars (app_id, key, value_encrypted) VALUES (?, ?, ?)";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(ctx->db->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, app.id);
        sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
        if (encrypted && encrypted_len > 0) {
            sqlite3_bind_blob(stmt, 3, encrypted, (int)encrypted_len, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, 3);
        }
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    free(encrypted);

    /* Return updated env list */
    char *html = vekd_tpl_env_partial(ctx->db, app.id);
    send_partial(server, conn, html);
}

/* ---- Route: DELETE /apps/:id/env/:key ---- */

static void handle_apps_env_del(HttpServer *server, Connection *conn,
                                VekdWebContext *ctx, HttpRequest *req,
                                RouteParams *params) {
    if (!check_auth(ctx, req)) {
        http_response_send(server, conn, 401, NULL, "text/plain",
                           "Unauthorized", 12, false);
        return;
    }

    VekdApp app;
    if (get_app_by_id_param(ctx, params, &app) < 0) {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "App not found", 13, false);
        return;
    }

    uint32_t key_len = 0;
    const char *key_val = get_env_key_param(params, &key_len);
    if (!key_val || key_len == 0) {
        http_response_send(server, conn, 400, NULL, "text/plain",
                           "Key is required", 15, false);
        return;
    }

    char key_buf[256];
    if (key_len >= sizeof(key_buf)) key_len = sizeof(key_buf) - 1;
    memcpy(key_buf, key_val, key_len);
    key_buf[key_len] = '\0';

    const char *sql = "DELETE FROM env_vars WHERE app_id = ? AND key = ?";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(ctx->db->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, app.id);
        sqlite3_bind_text(stmt, 2, key_buf, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* Return updated env list */
    char *html = vekd_tpl_env_partial(ctx->db, app.id);
    send_partial(server, conn, html);
}

/* ---- Route: GET /apps/:id/logs ---- */

static void handle_apps_logs(HttpServer *server, Connection *conn,
                             VekdWebContext *ctx, HttpRequest *req,
                             RouteParams *params) {
    if (!check_auth(ctx, req)) {
        http_response_send(server, conn, 401, NULL, "text/plain",
                           "Unauthorized", 12, false);
        return;
    }

    VekdApp app;
    if (get_app_by_id_param(ctx, params, &app) < 0) {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "App not found", 13, false);
        return;
    }

    char *html = vekd_tpl_logs_partial(ctx->db, app.id);
    send_partial(server, conn, html);
}

/* ---- Route: GET /settings/users ---- */

static void handle_settings_users_get(HttpServer *server, Connection *conn,
                                      VekdWebContext *ctx, HttpRequest *req,
                                      RouteParams *params) {
    (void)params;
    if (!check_auth(ctx, req)) {
        send_redirect(server, conn, "/login", NULL);
        return;
    }
    char *html = vekd_tpl_settings_users(ctx->db);
    send_html(server, conn, html);
}

/* ---- Route: POST /settings/users ---- */

static void handle_settings_users_post(HttpServer *server, Connection *conn,
                                       VekdWebContext *ctx, HttpRequest *req,
                                       RouteParams *params) {
    (void)params;
    if (!check_auth(ctx, req)) {
        send_redirect(server, conn, "/login", NULL);
        return;
    }

    FormData form;
    parse_form_body(req->body, req->body_len, &form);

    const char *email = form_get(&form, "email");
    const char *password = form_get(&form, "password");

    if (!email || !email[0] || !password || !password[0]) {
        send_redirect(server, conn, "/settings/users", NULL);
        return;
    }

    char pw_hash[128];
    vekd_crypto_hash_password(password, pw_hash, sizeof(pw_hash));
    vekd_db_user_create(ctx->db, email, pw_hash, 0);

    send_redirect(server, conn, "/settings/users", NULL);
}

/* ---- Route: GET /settings/cloudflare ---- */

static void handle_settings_cf_get(HttpServer *server, Connection *conn,
                                   VekdWebContext *ctx, HttpRequest *req,
                                   RouteParams *params) {
    (void)params;
    if (!check_auth(ctx, req)) {
        send_redirect(server, conn, "/login", NULL);
        return;
    }
    char *html = vekd_tpl_settings_cloudflare(ctx->db);
    send_html(server, conn, html);
}

/* ---- Route: POST /settings/cloudflare ---- */

static void handle_settings_cf_post(HttpServer *server, Connection *conn,
                                    VekdWebContext *ctx, HttpRequest *req,
                                    RouteParams *params) {
    (void)params;
    if (!check_auth(ctx, req)) {
        send_redirect(server, conn, "/login", NULL);
        return;
    }

    FormData form;
    parse_form_body(req->body, req->body_len, &form);

    const char *token = form_get(&form, "token");
    if (!token || !token[0]) {
        send_redirect(server, conn, "/settings/cloudflare", NULL);
        return;
    }

    /* Encrypt and store as a secret */
    size_t token_len = strlen(token);
    size_t enc_len = VEKD_NONCE_SIZE + token_len;
    uint8_t *encrypted = malloc(enc_len);
    if (encrypted) {
        vekd_crypto_encrypt(ctx->master_key, (const uint8_t *)token,
                            token_len, encrypted);

        const char *sql =
            "INSERT OR REPLACE INTO secrets (name, value_encrypted) VALUES ('cloudflare_token', ?)";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(ctx->db->db, sql, -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_blob(stmt, 1, encrypted, (int)enc_len, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        free(encrypted);
    }

    send_redirect(server, conn, "/settings/cloudflare", NULL);
}

/* ---- Route: GET /settings/backup ---- */

static void handle_settings_backup_get(HttpServer *server, Connection *conn,
                                       VekdWebContext *ctx, HttpRequest *req,
                                       RouteParams *params) {
    (void)params;
    if (!check_auth(ctx, req)) {
        send_redirect(server, conn, "/login", NULL);
        return;
    }
    char *html = vekd_tpl_settings_backup();
    send_html(server, conn, html);
}

/* ---- Route: POST /settings/backup ---- */

static void handle_settings_backup_post(HttpServer *server, Connection *conn,
                                        VekdWebContext *ctx, HttpRequest *req,
                                        RouteParams *params) {
    (void)params;
    if (!check_auth(ctx, req)) {
        send_redirect(server, conn, "/login", NULL);
        return;
    }

    /* Create a backup by copying the database file */
    char backup_path[512];
    snprintf(backup_path, sizeof(backup_path), "%s/backup_%lld.db",
             VEKD_DATA_DIR, (long long)time(NULL));

    /* Use SQLite backup API */
    sqlite3 *backup_db;
    int rc = sqlite3_open(backup_path, &backup_db);
    if (rc == SQLITE_OK) {
        sqlite3_backup *backup = sqlite3_backup_init(backup_db, "main",
                                                     ctx->db->db, "main");
        if (backup) {
            sqlite3_backup_step(backup, -1);
            sqlite3_backup_finish(backup);
        }
        sqlite3_close(backup_db);
    }

    send_redirect(server, conn, "/settings/backup", NULL);
}

/* ---- Main handler dispatch ---- */

static void vekd_web_handler(HttpServer *server, Connection *conn,
                             HttpRequest *req, RouteParams *params,
                             void *userdata) {
    VekdWebContext *ctx = (VekdWebContext *)server->handler_userdata;
    (void)userdata;

    /* The router matched a route - userdata from router_add is in
     * the 'userdata' param (via handler_data in http_server_add_route).
     * But our handler is the global one, so we use 'userdata' as the route ID. */
    void *route_id = userdata;

    if (route_id == ROUTE_ROOT) {
        handle_root(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_LOGIN_GET) {
        handle_login_get(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_LOGIN_POST) {
        handle_login_post(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_LOGOUT) {
        handle_logout(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_DASHBOARD) {
        handle_dashboard(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_LIST) {
        handle_apps_list(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_NEW) {
        handle_apps_new(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_CREATE) {
        handle_apps_create(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_DETAIL) {
        handle_apps_detail(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_DEPLOY) {
        handle_apps_deploy(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_START) {
        handle_apps_start(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_STOP) {
        handle_apps_stop(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_RESTART) {
        handle_apps_restart(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_DELETE) {
        handle_apps_delete(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_ENV_GET) {
        handle_apps_env_get(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_ENV_SET) {
        handle_apps_env_set(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_ENV_DEL) {
        handle_apps_env_del(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_APPS_LOGS) {
        handle_apps_logs(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_SETTINGS_USERS_GET) {
        handle_settings_users_get(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_SETTINGS_USERS_POST) {
        handle_settings_users_post(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_SETTINGS_CF_GET) {
        handle_settings_cf_get(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_SETTINGS_CF_POST) {
        handle_settings_cf_post(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_SETTINGS_BACKUP_GET) {
        handle_settings_backup_get(server, conn, ctx, req, params);
    } else if (route_id == ROUTE_SETTINGS_BACKUP_POST) {
        handle_settings_backup_post(server, conn, ctx, req, params);
    } else {
        http_response_send(server, conn, 404, NULL, "text/plain",
                           "Not Found", 9, false);
    }
}

/* ---- Public API ---- */

void vekd_web_init(HttpServer *server, VekdWebContext *ctx) {
    /* Initialize session store */
    vekd_session_init(&ctx->sessions);

    /* Initialize next port counter from database (issue #4: survive restarts) */
    ctx->next_port = vekd_db_get_next_port(ctx->db);

    /* Set the handler */
    http_server_set_handler(server, vekd_web_handler, ctx);

    /* Register routes */
    http_server_add_route(server, HTTP_GET, "/", ROUTE_ROOT);
    http_server_add_route(server, HTTP_GET, "/login", ROUTE_LOGIN_GET);
    http_server_add_route(server, HTTP_POST, "/login", ROUTE_LOGIN_POST);
    http_server_add_route(server, HTTP_GET, "/logout", ROUTE_LOGOUT);
    http_server_add_route(server, HTTP_GET, "/dashboard", ROUTE_DASHBOARD);
    http_server_add_route(server, HTTP_GET, "/apps/list", ROUTE_APPS_LIST);
    http_server_add_route(server, HTTP_GET, "/apps/new", ROUTE_APPS_NEW);
    http_server_add_route(server, HTTP_POST, "/apps", ROUTE_APPS_CREATE);
    http_server_add_route(server, HTTP_GET, "/apps/:id", ROUTE_APPS_DETAIL);
    http_server_add_route(server, HTTP_POST, "/apps/:id/deploy", ROUTE_APPS_DEPLOY);
    http_server_add_route(server, HTTP_POST, "/apps/:id/start", ROUTE_APPS_START);
    http_server_add_route(server, HTTP_POST, "/apps/:id/stop", ROUTE_APPS_STOP);
    http_server_add_route(server, HTTP_POST, "/apps/:id/restart", ROUTE_APPS_RESTART);
    http_server_add_route(server, HTTP_DELETE, "/apps/:id", ROUTE_APPS_DELETE);
    http_server_add_route(server, HTTP_GET, "/apps/:id/env", ROUTE_APPS_ENV_GET);
    http_server_add_route(server, HTTP_POST, "/apps/:id/env", ROUTE_APPS_ENV_SET);
    http_server_add_route(server, HTTP_DELETE, "/apps/:id/env/:key", ROUTE_APPS_ENV_DEL);
    http_server_add_route(server, HTTP_GET, "/apps/:id/logs", ROUTE_APPS_LOGS);
    http_server_add_route(server, HTTP_GET, "/settings/users", ROUTE_SETTINGS_USERS_GET);
    http_server_add_route(server, HTTP_POST, "/settings/users", ROUTE_SETTINGS_USERS_POST);
    http_server_add_route(server, HTTP_GET, "/settings/cloudflare", ROUTE_SETTINGS_CF_GET);
    http_server_add_route(server, HTTP_POST, "/settings/cloudflare", ROUTE_SETTINGS_CF_POST);
    http_server_add_route(server, HTTP_GET, "/settings/backup", ROUTE_SETTINGS_BACKUP_GET);
    http_server_add_route(server, HTTP_POST, "/settings/backup", ROUTE_SETTINGS_BACKUP_POST);
}
