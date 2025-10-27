/**
 * logger.c - Thread-safe file logging implementation
 */

#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

// Global state
static FILE *g_log_file = NULL;
static LogLevel g_min_level = LOG_LEVEL_INFO;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_log_path[512] = {0};
static long g_max_size_bytes = 10 * 1024 * 1024;  // 10 MB default
static int g_max_backups = 5;  // Keep 5 backup files
static char g_session_id[64] = {0};  // Session ID for log tagging

// Level names for output
static const char *level_names[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

/**
 * Create directory recursively (like mkdir -p)
 */
static int mkdir_p(const char *path) {
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    // Remove trailing slash
    if (tmp[len - 1] == '/') {
        tmp[len - 1] = 0;
    }

    // Create directories recursively
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
}

/**
 * Get default log file path
 * Priority: ~/.local/share/claude-c/logs/claude.log
 * Fallback: /tmp/claude-c.log
 */
static int get_default_log_path(char *buffer, size_t buffer_size) {
    const char *home = getenv("HOME");

    if (home) {
        // Try ~/.local/share/claude-c/logs/
        snprintf(buffer, buffer_size, "%s/.local/share/claude-c/logs", home);

        if (mkdir_p(buffer) == 0) {
            snprintf(buffer, buffer_size, "%s/.local/share/claude-c/logs/claude.log", home);
            return 0;
        }
    }

    // Fallback to /tmp
    snprintf(buffer, buffer_size, "/tmp/claude-c.log");
    return 0;
}

/**
 * Get current timestamp in ISO 8601 format
 */
static void get_timestamp(char *buffer, size_t buffer_size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/**
 * Extract just the filename from a full path
 */
static const char *get_filename(const char *path) {
    const char *last_slash = strrchr(path, '/');
    return last_slash ? last_slash + 1 : path;
}

/**
 * Get current log file size in bytes
 */
static long get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_size;
    }
    return 0;
}

/**
 * Perform log rotation
 * Renames current log to .1, .1 to .2, etc.
 * Deletes oldest backup if max_backups is exceeded
 */
static void rotate_log(void) {
    char old_name[600];
    char new_name[600];

    // Close current log file
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    // Delete oldest backup if it exists
    snprintf(old_name, sizeof(old_name), "%s.%d", g_log_path, g_max_backups);
    unlink(old_name);  // Ignore errors if file doesn't exist

    // Rotate existing backups: .N-1 -> .N
    for (int i = g_max_backups - 1; i >= 1; i--) {
        snprintf(old_name, sizeof(old_name), "%s.%d", g_log_path, i);
        snprintf(new_name, sizeof(new_name), "%s.%d", g_log_path, i + 1);
        rename(old_name, new_name);  // Ignore errors
    }

    // Rotate current log to .1
    snprintf(new_name, sizeof(new_name), "%s.1", g_log_path);
    rename(g_log_path, new_name);

    // Reopen log file (creates new empty file)
    g_log_file = fopen(g_log_path, "a");

    // Write rotation marker
    if (g_log_file) {
        char timestamp[64];
        get_timestamp(timestamp, sizeof(timestamp));
        fprintf(g_log_file, "=== Log rotated: %s ===\n", timestamp);
        fflush(g_log_file);
    }
}

/**
 * Check if rotation is needed and perform it
 * Called before each log write (already inside mutex)
 */
static void check_and_rotate(void) {
    if (!g_log_file || g_log_path[0] == '\0') {
        return;
    }

    long current_size = get_file_size(g_log_path);
    if (current_size >= g_max_size_bytes) {
        rotate_log();
    }
}

int log_init(void) {
    char log_path[512];

    if (get_default_log_path(log_path, sizeof(log_path)) != 0) {
        return -1;
    }

    return log_init_with_path(log_path);
}

int log_init_with_path(const char *log_path) {
    pthread_mutex_lock(&g_log_mutex);

    // Close existing log file if open
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    // Open log file in append mode
    g_log_file = fopen(log_path, "a");
    if (!g_log_file) {
        pthread_mutex_unlock(&g_log_mutex);
        fprintf(stderr, "Failed to open log file: %s\n", log_path);
        return -1;
    }

    // Store path for reference
    strncpy(g_log_path, log_path, sizeof(g_log_path) - 1);
    g_log_path[sizeof(g_log_path) - 1] = '\0';

    // Write startup marker
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    fprintf(g_log_file, "\n=== Log started: %s (PID: %d) ===\n", timestamp, getpid());
    fflush(g_log_file);

    pthread_mutex_unlock(&g_log_mutex);
    return 0;
}

void log_set_level(LogLevel level) {
    pthread_mutex_lock(&g_log_mutex);
    g_min_level = level;
    pthread_mutex_unlock(&g_log_mutex);
}

void log_set_rotation(int max_size_mb, int max_backups) {
    pthread_mutex_lock(&g_log_mutex);
    g_max_size_bytes = (long)max_size_mb * 1024 * 1024;
    g_max_backups = max_backups;
    pthread_mutex_unlock(&g_log_mutex);
}

void log_set_session_id(const char *session_id) {
    pthread_mutex_lock(&g_log_mutex);
    if (session_id) {
        strncpy(g_session_id, session_id, sizeof(g_session_id) - 1);
        g_session_id[sizeof(g_session_id) - 1] = '\0';
    } else {
        g_session_id[0] = '\0';
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void log_message(LogLevel level, const char *file, int line,
                const char *func, const char *fmt, ...) {
    // Quick check without lock for performance
    if (level < g_min_level) {
        return;
    }

    pthread_mutex_lock(&g_log_mutex);

    if (!g_log_file) {
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }

    // Check if rotation is needed
    check_and_rotate();

    if (!g_log_file) {  // Rotation might have failed
        pthread_mutex_unlock(&g_log_mutex);
        return;
    }

    // Get timestamp
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    // Format: [TIMESTAMP] [SESSION_ID] LEVEL [file:line] function: message
    if (g_session_id[0] != '\0') {
        fprintf(g_log_file, "[%s] [%s] %-5s [%s:%d] %s: ",
                timestamp,
                g_session_id,
                level_names[level],
                get_filename(file),
                line,
                func);
    } else {
        // Fallback format without session ID
        fprintf(g_log_file, "[%s] %-5s [%s:%d] %s: ",
                timestamp,
                level_names[level],
                get_filename(file),
                line,
                func);
    }

    // Write the actual log message
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);

    fprintf(g_log_file, "\n");

    // Auto-flush on WARN and ERROR for immediate visibility
    if (level >= LOG_LEVEL_WARN) {
        fflush(g_log_file);
    }

    pthread_mutex_unlock(&g_log_mutex);
}

void log_flush(void) {
    pthread_mutex_lock(&g_log_mutex);

    if (g_log_file) {
        fflush(g_log_file);
    }

    pthread_mutex_unlock(&g_log_mutex);
}

void log_shutdown(void) {
    pthread_mutex_lock(&g_log_mutex);

    if (g_log_file) {
        char timestamp[64];
        get_timestamp(timestamp, sizeof(timestamp));
        fprintf(g_log_file, "=== Log ended: %s ===\n\n", timestamp);
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }

    pthread_mutex_unlock(&g_log_mutex);
}
