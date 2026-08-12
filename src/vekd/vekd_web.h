/*
 * vekd_web.h - Web UI HTTP server and route handlers for vekd.
 *
 * Provides the htmx-based admin dashboard served on port 8080.
 * This is the only management interface for vekd - all app deployment,
 * configuration, and monitoring happens through this web UI.
 */
#ifndef VEKD_WEB_H
#define VEKD_WEB_H

#include "../http_server.h"
#include "vekd_db.h"
#include "vekd_supervisor.h"
#include "vekd_session.h"
#include <stdint.h>

/* Web server context passed as handler userdata */
typedef struct {
    VekdDB *db;
    VekdSupervisor *supervisor;
    VekdSessionStore sessions;
    uint8_t *master_key;
    int next_port;  /* Next port to assign to an app */
} VekdWebContext;

/* Initialize the web server routes on the given HttpServer.
 * Sets up all routes and the handler function. */
void vekd_web_init(HttpServer *server, VekdWebContext *ctx);

#endif /* VEKD_WEB_H */
