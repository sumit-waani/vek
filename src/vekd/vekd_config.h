/*
 * vekd_config.h - Constants and configuration for the vekd daemon.
 *
 * vekd is a long-running system service that provides a web dashboard
 * for managing production deployments. It listens on port 8080 and
 * stores state in a SQLite database.
 */
#ifndef VEKD_CONFIG_H
#define VEKD_CONFIG_H

/* Network */
#define VEKD_DEFAULT_PORT       8080
#define VEKD_APP_PORT_MIN       10000
#define VEKD_APP_PORT_MAX       19999

/* Filesystem paths */
#define VEKD_DATA_DIR           "/var/lib/vek/"
#define VEKD_DB_PATH            "/var/lib/vek/vekd.db"
#define VEKD_MASTER_KEY_PATH    "/var/lib/vek/master.key"
#define VEKD_APPS_DIR           "/var/lib/vek/apps/"

/* Master key */
#define VEKD_MASTER_KEY_SIZE    32  /* 256-bit key */
#define VEKD_MASTER_KEY_MODE    0600

/* Supervisor */
#define VEKD_RESTART_DELAY_MS   1000
#define VEKD_MAX_RESTART_COUNT  10
#define VEKD_RESTART_WINDOW_SEC 60

/* Version */
#define VEKD_VERSION            "0.1.0"

#endif /* VEKD_CONFIG_H */
