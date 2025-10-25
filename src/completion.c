/*
 * completion.c - Path and File Completion Implementation
 */

#include "completion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <libgen.h>

// ============================================================================
// Helper Functions
// ============================================================================

// Split a path into directory and basename parts
// Returns: 0 on success, -1 on error
// dir_out and base_out must be at least PATH_MAX bytes
static int split_path(const char *path, char *dir_out, char *base_out) {
    if (strlen(path) == 0) {
        // Empty path: complete in current directory
        strcpy(dir_out, ".");
        strcpy(base_out, "");
        return 0;
    }

    // Make a copy since dirname/basename may modify the string
    char path_copy1[PATH_MAX];
    char path_copy2[PATH_MAX];
    strncpy(path_copy1, path, PATH_MAX - 1);
    strncpy(path_copy2, path, PATH_MAX - 1);
    path_copy1[PATH_MAX - 1] = '\0';
    path_copy2[PATH_MAX - 1] = '\0';

    // Find the last '/' to split directory and basename
    char *last_slash = strrchr(path_copy1, '/');

    if (last_slash == NULL) {
        // No slash: complete in current directory
        strcpy(dir_out, ".");
        strncpy(base_out, path, PATH_MAX - 1);
        base_out[PATH_MAX - 1] = '\0';
    } else if (last_slash == path_copy1) {
        // Path starts with '/': root directory
        strcpy(dir_out, "/");
        strncpy(base_out, path + 1, PATH_MAX - 1);
        base_out[PATH_MAX - 1] = '\0';
    } else {
        // Normal path: split at last slash
        *last_slash = '\0';
        strncpy(dir_out, path_copy1, PATH_MAX - 1);
        dir_out[PATH_MAX - 1] = '\0';
        strncpy(base_out, last_slash + 1, PATH_MAX - 1);
        base_out[PATH_MAX - 1] = '\0';
    }

    return 0;
}

// Check if an entry matches the prefix
static int matches_prefix(const char *entry, const char *prefix) {
    size_t prefix_len = strlen(prefix);
    if (prefix_len == 0) {
        return 1;  // Empty prefix matches everything
    }
    return strncmp(entry, prefix, prefix_len) == 0;
}

// ============================================================================
// Generic Path Completion
// ============================================================================

static CompletionResult* complete_path_internal(const char *partial, int dirs_only) {
    char dir_path[PATH_MAX];
    char prefix[PATH_MAX];

    // Split partial into directory and basename
    if (split_path(partial, dir_path, prefix) != 0) {
        return NULL;
    }

    // Open directory
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return NULL;  // Can't open directory
    }

    // Allocate result structure
    CompletionResult *result = calloc(1, sizeof(CompletionResult));
    if (!result) {
        closedir(dir);
        return NULL;
    }

    result->options = NULL;
    result->count = 0;
    result->selected = 0;

    // Initial capacity for options array
    int capacity = 16;
    result->options = malloc(capacity * sizeof(char*));
    if (!result->options) {
        free(result);
        closedir(dir);
        return NULL;
    }

    // Read directory entries
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Check if entry matches prefix
        if (!matches_prefix(entry->d_name, prefix)) {
            continue;
        }

        // Build full path to check if it's a directory
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;  // Can't stat, skip
        }

        // If dirs_only is set, skip non-directories
        if (dirs_only && !S_ISDIR(st.st_mode)) {
            continue;
        }

        // Build the completion string
        // If the original partial had a path component, include it
        char completion[PATH_MAX];
        if (strcmp(dir_path, ".") == 0) {
            // No directory component, just the name
            snprintf(completion, sizeof(completion), "%s", entry->d_name);
        } else {
            // Include directory component
            snprintf(completion, sizeof(completion), "%s/%s", dir_path, entry->d_name);
        }

        // Add trailing slash for directories
        if (S_ISDIR(st.st_mode)) {
            size_t len = strlen(completion);
            if (len < PATH_MAX - 1) {
                completion[len] = '/';
                completion[len + 1] = '\0';
            }
        }

        // Expand array if needed
        if (result->count >= capacity) {
            capacity *= 2;
            char **new_options = realloc(result->options, capacity * sizeof(char*));
            if (!new_options) {
                // Cleanup and return what we have
                closedir(dir);
                return result;
            }
            result->options = new_options;
        }

        // Add to results
        result->options[result->count] = strdup(completion);
        if (result->options[result->count]) {
            result->count++;
        }
    }

    closedir(dir);

    // If no matches, return NULL
    if (result->count == 0) {
        free(result->options);
        free(result);
        return NULL;
    }

    return result;
}

// ============================================================================
// API Implementation
// ============================================================================

CompletionResult* complete_filepath(const char *partial, void *ctx) {
    (void)ctx;  // Unused for now
    return complete_path_internal(partial, 0);  // Include files and directories
}

CompletionResult* complete_dirpath(const char *partial, void *ctx) {
    (void)ctx;  // Unused for now
    return complete_path_internal(partial, 1);  // Directories only
}
