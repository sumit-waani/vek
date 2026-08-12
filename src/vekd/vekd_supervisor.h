/*
 * vekd_supervisor.h - Process supervisor for managed applications.
 *
 * Manages app processes via fork/exec with optional systemd-run integration.
 * Monitors children with waitpid and restarts on crash.
 */
#ifndef VEKD_SUPERVISOR_H
#define VEKD_SUPERVISOR_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>

#define VEKD_MAX_PROCESSES 64

/* State of a supervised process */
typedef enum {
    VEKD_PROC_STOPPED = 0,
    VEKD_PROC_RUNNING,
    VEKD_PROC_CRASHED,
    VEKD_PROC_RESTARTING
} VekdProcState;

/* A supervised process entry */
typedef struct {
    int64_t app_id;
    pid_t pid;
    VekdProcState state;
    int restart_count;
    int64_t last_start_time;
    int64_t last_crash_time;
    char command[1024];
    char working_dir[512];
} VekdProcess;

/* The supervisor context */
typedef struct {
    VekdProcess processes[VEKD_MAX_PROCESSES];
    int process_count;
    bool running;
    bool use_systemd;
} VekdSupervisor;

/* Initialize the supervisor */
void vekd_supervisor_init(VekdSupervisor *sup);

/* Check if systemd is available */
bool vekd_supervisor_detect_systemd(void);

/* Start a process under supervision.
 * Returns the index in the process table, or -1 on error. */
int vekd_supervisor_start(VekdSupervisor *sup, int64_t app_id,
                          const char *command, const char *working_dir);

/* Stop a supervised process by app_id. Returns 0 on success. */
int vekd_supervisor_stop(VekdSupervisor *sup, int64_t app_id);

/* Check on child processes - call periodically.
 * Handles SIGCHLD, detects crashed processes, and triggers restarts.
 * Returns number of processes restarted. */
int vekd_supervisor_check(VekdSupervisor *sup);

/* Stop all supervised processes */
void vekd_supervisor_shutdown(VekdSupervisor *sup);

/* Get process state for an app_id. Returns NULL if not found. */
VekdProcess *vekd_supervisor_get(VekdSupervisor *sup, int64_t app_id);

#endif /* VEKD_SUPERVISOR_H */
