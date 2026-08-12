/*
 * vekd_supervisor.c - Process supervisor implementation.
 *
 * Manages child processes via fork/exec, monitors them with waitpid,
 * and restarts crashed processes with backoff logic.
 */
#define _GNU_SOURCE
#include "vekd_supervisor.h"
#include "vekd_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <errno.h>

void vekd_supervisor_init(VekdSupervisor *sup) {
    memset(sup, 0, sizeof(VekdSupervisor));
    sup->running = true;
    sup->use_systemd = vekd_supervisor_detect_systemd();
}

bool vekd_supervisor_detect_systemd(void) {
    /* Check if systemd-run is available */
    return access("/usr/bin/systemd-run", X_OK) == 0 ||
           access("/bin/systemd-run", X_OK) == 0;
}

static int64_t current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

static pid_t fork_exec_process(const char *command, const char *working_dir) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "vekd: fork failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process */
        if (working_dir && working_dir[0]) {
            if (chdir(working_dir) < 0) {
                _exit(127);
            }
        }

        /* Execute via shell for command parsing */
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    return pid;
}

int vekd_supervisor_start(VekdSupervisor *sup, int64_t app_id,
                          const char *command, const char *working_dir) {
    if (sup->process_count >= VEKD_MAX_PROCESSES) {
        fprintf(stderr, "vekd: supervisor full, cannot start more processes\n");
        return -1;
    }

    /* Check if already running */
    for (int i = 0; i < sup->process_count; i++) {
        if (sup->processes[i].app_id == app_id &&
            sup->processes[i].state == VEKD_PROC_RUNNING) {
            fprintf(stderr, "vekd: app %lld already running\n", (long long)app_id);
            return -1;
        }
    }

    pid_t pid = fork_exec_process(command, working_dir);
    if (pid < 0) return -1;

    /* Find an existing slot or use a new one */
    int idx = -1;
    for (int i = 0; i < sup->process_count; i++) {
        if (sup->processes[i].app_id == app_id) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        idx = sup->process_count++;
    }

    VekdProcess *proc = &sup->processes[idx];
    proc->app_id = app_id;
    proc->pid = pid;
    proc->state = VEKD_PROC_RUNNING;
    proc->last_start_time = current_time_ms();
    strncpy(proc->command, command, sizeof(proc->command) - 1);
    proc->command[sizeof(proc->command) - 1] = '\0';
    if (working_dir) {
        strncpy(proc->working_dir, working_dir, sizeof(proc->working_dir) - 1);
        proc->working_dir[sizeof(proc->working_dir) - 1] = '\0';
    }

    return idx;
}

int vekd_supervisor_stop(VekdSupervisor *sup, int64_t app_id) {
    VekdProcess *proc = vekd_supervisor_get(sup, app_id);
    if (!proc) return -1;

    if (proc->state == VEKD_PROC_RUNNING && proc->pid > 0) {
        kill(proc->pid, SIGTERM);

        /* Give it a moment to terminate */
        usleep(100000); /* 100ms */

        /* Check if still alive */
        int status;
        pid_t result = waitpid(proc->pid, &status, WNOHANG);
        if (result == 0) {
            /* Still running, force kill */
            kill(proc->pid, SIGKILL);
            waitpid(proc->pid, &status, 0);
        }
    }

    proc->state = VEKD_PROC_STOPPED;
    proc->pid = 0;
    return 0;
}

int vekd_supervisor_check(VekdSupervisor *sup) {
    int restarted = 0;

    for (int i = 0; i < sup->process_count; i++) {
        VekdProcess *proc = &sup->processes[i];
        if (proc->state != VEKD_PROC_RUNNING) continue;
        if (proc->pid <= 0) continue;

        int status;
        pid_t result = waitpid(proc->pid, &status, WNOHANG);
        if (result <= 0) continue; /* Still running or error */

        /* Process exited */
        proc->last_crash_time = current_time_ms();

        /* Check restart window */
        int64_t elapsed = proc->last_crash_time - proc->last_start_time;
        if (elapsed < VEKD_RESTART_WINDOW_SEC * 1000) {
            proc->restart_count++;
        } else {
            proc->restart_count = 1;
        }

        if (proc->restart_count > VEKD_MAX_RESTART_COUNT) {
            proc->state = VEKD_PROC_CRASHED;
            fprintf(stderr, "vekd: app %lld exceeded max restarts, marking crashed\n",
                    (long long)proc->app_id);
            continue;
        }

        /* Restart the process */
        proc->state = VEKD_PROC_RESTARTING;
        pid_t new_pid = fork_exec_process(proc->command, proc->working_dir);
        if (new_pid > 0) {
            proc->pid = new_pid;
            proc->state = VEKD_PROC_RUNNING;
            proc->last_start_time = current_time_ms();
            restarted++;
        } else {
            proc->state = VEKD_PROC_CRASHED;
        }
    }

    return restarted;
}

void vekd_supervisor_shutdown(VekdSupervisor *sup) {
    sup->running = false;
    for (int i = 0; i < sup->process_count; i++) {
        if (sup->processes[i].state == VEKD_PROC_RUNNING) {
            vekd_supervisor_stop(sup, sup->processes[i].app_id);
        }
    }
}

VekdProcess *vekd_supervisor_get(VekdSupervisor *sup, int64_t app_id) {
    for (int i = 0; i < sup->process_count; i++) {
        if (sup->processes[i].app_id == app_id) {
            return &sup->processes[i];
        }
    }
    return NULL;
}
