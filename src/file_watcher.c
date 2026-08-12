#define _GNU_SOURCE
#include "file_watcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <poll.h>
#include <errno.h>

#define MAX_WATCHES 1024
#define EVENT_BUF_SIZE 4096

typedef struct {
    int wd;
    char path[4096];
} WatchEntry;

struct FileWatcher {
    int inotify_fd;
    WatchEntry watches[MAX_WATCHES];
    int watch_count;
};

// Check if a directory name should be skipped
static bool should_skip_dir(const char* name) {
    if (strcmp(name, ".git") == 0) return true;
    if (strcmp(name, "build") == 0) return true;
    if (strcmp(name, "node_modules") == 0) return true;
    return false;
}

// Check if a filename ends with .ve
static bool is_ve_file(const char* name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    return strcmp(name + len - 3, ".ve") == 0;
}

FileWatcher* file_watcher_create(void) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    FileWatcher* fw = (FileWatcher*)malloc(sizeof(FileWatcher));
    if (fw == NULL) {
        close(fd);
        return NULL;
    }

    fw->inotify_fd = fd;
    fw->watch_count = 0;
    return fw;
}

bool file_watcher_add_dir(FileWatcher* fw, const char* path) {
    if (fw->watch_count >= MAX_WATCHES) {
        return false;
    }

    // Add watch on this directory
    int wd = inotify_add_watch(fw->inotify_fd, path,
                               IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO);
    if (wd < 0) {
        fprintf(stderr, "Warning: could not watch '%s': %s\n", path, strerror(errno));
        return false;
    }

    // Store the watch entry
    WatchEntry* entry = &fw->watches[fw->watch_count];
    entry->wd = wd;
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    fw->watch_count++;

    // Recursively add subdirectories
    DIR* dir = opendir(path);
    if (dir == NULL) {
        return true; // watch was added, just can't recurse
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' ||
            (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue; // skip . and ..
        }

        if (ent->d_type != DT_DIR) {
            continue;
        }

        if (should_skip_dir(ent->d_name)) {
            continue;
        }

        char subpath[4096];
        snprintf(subpath, sizeof(subpath), "%s/%s", path, ent->d_name);
        file_watcher_add_dir(fw, subpath);
    }

    closedir(dir);
    return true;
}

// Find the directory path for a watch descriptor
static const char* find_watch_path(FileWatcher* fw, int wd) {
    for (int i = 0; i < fw->watch_count; i++) {
        if (fw->watches[i].wd == wd) {
            return fw->watches[i].path;
        }
    }
    return NULL;
}

int file_watcher_poll(FileWatcher* fw, char* changed_path, size_t path_size, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = fw->inotify_fd;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) return 0; // interrupted, treat as timeout
        return -1;
    }
    if (ret == 0) {
        return 0; // timeout
    }

    // Read events
    char buf[EVENT_BUF_SIZE]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    ssize_t len = read(fw->inotify_fd, buf, sizeof(buf));
    if (len < 0) {
        if (errno == EAGAIN) return 0;
        return -1;
    }

    // Process events, looking for .ve file changes
    char* ptr = buf;
    while (ptr < buf + len) {
        struct inotify_event* event = (struct inotify_event*)ptr;

        if (event->len > 0 && is_ve_file(event->name)) {
            const char* dir_path = find_watch_path(fw, event->wd);
            if (dir_path != NULL) {
                snprintf(changed_path, path_size, "%s/%s", dir_path, event->name);
                return 1;
            }
        }

        // If a new directory was created, add a watch on it
        if ((event->mask & IN_CREATE) && (event->mask & IN_ISDIR)) {
            if (event->len > 0 && !should_skip_dir(event->name)) {
                const char* dir_path = find_watch_path(fw, event->wd);
                if (dir_path != NULL) {
                    char new_dir[4096];
                    snprintf(new_dir, sizeof(new_dir), "%s/%s", dir_path, event->name);
                    file_watcher_add_dir(fw, new_dir);
                }
            }
        }

        ptr += sizeof(struct inotify_event) + event->len;
    }

    return 0; // no .ve file changes in this batch
}

void file_watcher_destroy(FileWatcher* fw) {
    if (fw == NULL) return;

    for (int i = 0; i < fw->watch_count; i++) {
        inotify_rm_watch(fw->inotify_fd, fw->watches[i].wd);
    }

    close(fw->inotify_fd);
    free(fw);
}
