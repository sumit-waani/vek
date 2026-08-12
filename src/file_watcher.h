#ifndef VEK_FILE_WATCHER_H
#define VEK_FILE_WATCHER_H

#include <stdbool.h>
#include <stddef.h>

// Opaque file watcher type using Linux inotify
typedef struct FileWatcher FileWatcher;

// Create a new file watcher (initializes inotify fd)
FileWatcher* file_watcher_create(void);

// Recursively add watches on a directory and its subdirectories.
// Skips .git/, build/, and node_modules/ directories.
bool file_watcher_add_dir(FileWatcher* fw, const char* path);

// Poll for file change events.
// Returns 1 if an event occurred (changed_path filled with the path),
// 0 if timeout expired, -1 on error.
// Only reports changes to .ve files.
int file_watcher_poll(FileWatcher* fw, char* changed_path, size_t path_size, int timeout_ms);

// Destroy the file watcher and free all resources.
void file_watcher_destroy(FileWatcher* fw);

#endif // VEK_FILE_WATCHER_H
