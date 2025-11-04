/*
 * Claude Code - Pure C Implementation
 * A lightweight coding agent that interacts with OpenAI-compatible APIs
 *
 * Compilation: make
 * Usage: ./claude "your prompt here"
 *
 * Dependencies: libcurl, cJSON, pthread
 */

#ifdef __APPLE__
    #define _DARWIN_C_SOURCE
#else
    #define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <errno.h>
#include <glob.h>
#include <regex.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <limits.h>
#include <libgen.h>
#include <dirent.h>
#include <termios.h>
#include <ctype.h>
#include <signal.h>
#include "colorscheme.h"
#include "fallback_colors.h"
#include "patch_parser.h"

#ifdef TEST_BUILD
// Disable unused function warnings for test builds since not all functions are used by tests
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
// Test build: stub out persistence (logger is linked via LOGGER_OBJ)
// Stub persistence types and functions
struct PersistenceDB { int dummy; };
static struct PersistenceDB* persistence_init(const char *path) { (void)path; return NULL; }
static void persistence_close(struct PersistenceDB *db) { (void)db; }
static void persistence_log_api_call(
    struct PersistenceDB *db,
    const char *session_id,
    const char *url,
    const char *request,
    const char *response,
    const char *model,
    const char *status,
    int code,
    const char *error_msg,
    long duration_ms,
    int tool_count
) { (void)db; (void)session_id; (void)url; (void)request; (void)response; (void)model; (void)status; (void)code; (void)error_msg; (void)duration_ms; (void)tool_count; }

// Stub Bedrock types and functions
typedef struct {
    char *access_key_id;
    char *secret_access_key;
    char *session_token;
    char *region;
    char *profile;
} AWSCredentials;

typedef struct BedrockConfigStruct {
    int enabled;
    char *region;
    char *model_id;
    char *endpoint;
    AWSCredentials *creds;
} BedrockConfig;

static int bedrock_is_enabled(void) { return 0; }
static BedrockConfig* bedrock_config_init(const char *model_id) { (void)model_id; return NULL; }
static void bedrock_config_free(BedrockConfig *config) { (void)config; }
static char* bedrock_convert_request(const char *openai_request) { (void)openai_request; return NULL; }
static cJSON* bedrock_convert_response(const char *bedrock_response) { (void)bedrock_response; return NULL; }
static struct curl_slist* bedrock_sign_request(
    struct curl_slist *headers,
    const char *method,
    const char *url,
    const char *payload,
    const AWSCredentials *creds,
    const char *region,
    const char *service
) { (void)method; (void)url; (void)payload; (void)creds; (void)region; (void)service; return headers; }
static int bedrock_handle_auth_error(BedrockConfig *config, long http_status, const char *error_message, const char *response_body) {
    (void)config; (void)http_status; (void)error_message; (void)response_body;
    return 0;
}
#else
// Normal build: use actual implementations
#include "logger.h"
#include "persistence.h"
#endif

// Visual indicators for interactive mode
#include "indicators.h"

// Internal API for module access
#include "claude_internal.h"
#include "provider.h"  // For ApiCallResult and Provider definitions
#include "todo.h"

// New input system modules
#include "lineedit.h"
#include "commands.h"

// TUI module
#include "tui.h"
#include "message_queue.h"
#include "ai_worker.h"

// AWS Bedrock support
#ifndef TEST_BUILD
#include "aws_bedrock.h"
#endif

#ifdef TEST_BUILD
#define main claude_main
#endif

// Version
// ============================================================================
// Output Helpers
// ============================================================================



static void print_assistant(const char *text) {
    // Use accent color for role name, foreground for main text
    char role_color_code[32];
    char text_color_code[32];
    const char *role_color_start;
    const char *text_color_start;

    // Get accent color for role name
    if (get_colorscheme_color(COLORSCHEME_ASSISTANT, role_color_code, sizeof(role_color_code)) == 0) {
        role_color_start = role_color_code;
    } else {
        LOG_WARN("Using fallback ANSI color for ASSISTANT");
        role_color_start = ANSI_FALLBACK_ASSISTANT;
    }

    // Get foreground color for main text
    if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
        text_color_start = text_color_code;
    } else {
        LOG_WARN("Using fallback ANSI color for FOREGROUND");
        text_color_start = ANSI_FALLBACK_FOREGROUND;
    }

    printf("%s[Assistant]%s %s%s%s\n", role_color_start, ANSI_RESET, text_color_start, text, ANSI_RESET);
    fflush(stdout);
}

static void print_tool(const char *tool_name, const char *details) {
    // Use accent color for tool indicator, foreground for details
    char tool_color_code[32];
    char text_color_code[32];
    const char *tool_color_start;
    const char *text_color_start;

    // Get accent color for tool indicator
    if (get_colorscheme_color(COLORSCHEME_TOOL, tool_color_code, sizeof(tool_color_code)) == 0) {
        tool_color_start = tool_color_code;
    } else {
        LOG_WARN("Using fallback ANSI color for TOOL");
        tool_color_start = ANSI_FALLBACK_TOOL;
    }

    // Get foreground color for details
    if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
        text_color_start = text_color_code;
    } else {
        LOG_WARN("Using fallback ANSI color for FOREGROUND");
        text_color_start = ANSI_FALLBACK_FOREGROUND;
    }

    printf("%s[Tool: %s]%s", tool_color_start, tool_name, ANSI_RESET);
    if (details && strlen(details) > 0) {
        printf(" %s%s%s", text_color_start, details, ANSI_RESET);
    }
    printf("\n");
    fflush(stdout);
}

static void print_error(const char *text);

static void ui_append_line(TUIState *tui,
                           TUIMessageQueue *queue,
                           const char *prefix,
                           const char *text,
                           TUIColorPair color) {
    const char *safe_text = text ? text : "";
    const char *safe_prefix = prefix ? prefix : "";

    if (tui) {
        tui_add_conversation_line(tui, safe_prefix, safe_text, color);
        return;
    }

    if (queue) {
        size_t prefix_len = safe_prefix[0] ? strlen(safe_prefix) : 0;
        size_t text_len = strlen(safe_text);
        size_t extra_space = (prefix_len > 0 && text_len > 0) ? 1 : 0;
        size_t total = prefix_len + extra_space + text_len + 1;

        char *formatted = malloc(total);
        if (!formatted) {
            LOG_ERROR("Failed to allocate memory for TUI message");
            return;
        }

        if (prefix_len > 0 && text_len > 0) {
            snprintf(formatted, total, "%s %s", safe_prefix, safe_text);
        } else if (prefix_len > 0) {
            snprintf(formatted, total, "%s", safe_prefix);
        } else {
            snprintf(formatted, total, "%s", safe_text);
        }

        post_tui_message(queue, TUI_MSG_ADD_LINE, formatted);
        free(formatted);
        return;
    }

    if (!tui && !queue) {
        if (strcmp(safe_prefix, "[Assistant]") == 0) {
            print_assistant(safe_text);
            return;
        }

        if (strncmp(safe_prefix, "[Tool", 5) == 0) {
            const char *colon = strchr(safe_prefix, ':');
            const char *close = strrchr(safe_prefix, ']');
            const char *name_start = NULL;
            size_t name_len = 0;
            if (colon) {
                name_start = colon + 1;
                if (*name_start == ' ') {
                    name_start++;
                }
                if (close && close > name_start) {
                    name_len = (size_t)(close - name_start);
                }
            }

            char tool_name[128];
            if (name_len == 0 || name_len >= sizeof(tool_name)) {
                snprintf(tool_name, sizeof(tool_name), "tool");
            } else {
                memcpy(tool_name, name_start, name_len);
                tool_name[name_len] = '\0';
            }
            print_tool(tool_name, safe_text);
            return;
        }

        if (strcmp(safe_prefix, "[Error]") == 0) {
            print_error(safe_text);
            return;
        }

        if (safe_prefix[0]) {
            printf("%s %s\n", safe_prefix, safe_text);
        } else {
            printf("%s\n", safe_text);
        }
        fflush(stdout);
        return;
    }

}

static void ui_set_status(TUIState *tui,
                          TUIMessageQueue *queue,
                          const char *status_text) {
    const char *safe = status_text ? status_text : "";
    if (tui) {
        tui_update_status(tui, safe);
        return;
    }
    if (queue) {
        post_tui_message(queue, TUI_MSG_STATUS, safe);
        return;
    }
    if (safe[0] != '\0') {
        printf("[Status] %s\n", safe);
    }
}

static void ui_show_error(TUIState *tui,
                          TUIMessageQueue *queue,
                          const char *error_text) {
    const char *safe = error_text ? error_text : "";
    if (tui) {
        tui_add_conversation_line(tui, "[Error]", safe, COLOR_PAIR_ERROR);
        return;
    }
    if (queue) {
        post_tui_message(queue, TUI_MSG_ERROR, safe);
        return;
    }
    print_error(safe);
}

// Helper function to extract tool details from arguments
static char* get_tool_details(const char *tool_name, cJSON *arguments) {
    if (!arguments || !cJSON_IsObject(arguments)) {
        return NULL;
    }

    static char details[256]; // static buffer for thread safety
    details[0] = '\0';

    if (strcmp(tool_name, "Bash") == 0) {
        cJSON *command = cJSON_GetObjectItem(arguments, "command");
        if (cJSON_IsString(command)) {
            const char *cmd = command->valuestring;
            // Truncate long commands to first 50 characters
            if (strlen(cmd) > 50) {
                snprintf(details, sizeof(details), "%.47s...", cmd);
            } else {
                strncpy(details, cmd, sizeof(details) - 1);
                details[sizeof(details) - 1] = '\0';
            }
        }
    } else if (strcmp(tool_name, "Read") == 0) {
        cJSON *file_path = cJSON_GetObjectItem(arguments, "file_path");
        cJSON *start_line = cJSON_GetObjectItem(arguments, "start_line");
        cJSON *end_line = cJSON_GetObjectItem(arguments, "end_line");

        if (cJSON_IsString(file_path)) {
            const char *path = file_path->valuestring;
            // Extract just the filename from the path
            const char *filename = strrchr(path, '/');
            filename = filename ? filename + 1 : path;

            if (cJSON_IsNumber(start_line) && cJSON_IsNumber(end_line)) {
                snprintf(details, sizeof(details), "%s:%d-%d", filename,
                        start_line->valueint, end_line->valueint);
            } else if (cJSON_IsNumber(start_line)) {
                snprintf(details, sizeof(details), "%s:%d", filename, start_line->valueint);
            } else {
                strncpy(details, filename, sizeof(details) - 1);
                details[sizeof(details) - 1] = '\0';
            }
        }
    } else if (strcmp(tool_name, "Write") == 0) {
        cJSON *file_path = cJSON_GetObjectItem(arguments, "file_path");
        if (cJSON_IsString(file_path)) {
            const char *path = file_path->valuestring;
            // Extract just the filename from the path
            const char *filename = strrchr(path, '/');
            filename = filename ? filename + 1 : path;
            strncpy(details, filename, sizeof(details) - 1);
            details[sizeof(details) - 1] = '\0';
        }
    } else if (strcmp(tool_name, "Edit") == 0) {
        cJSON *file_path = cJSON_GetObjectItem(arguments, "file_path");
        cJSON *use_regex = cJSON_GetObjectItem(arguments, "use_regex");

        if (cJSON_IsString(file_path)) {
            const char *path = file_path->valuestring;
            // Extract just the filename from the path
            const char *filename = strrchr(path, '/');
            filename = filename ? filename + 1 : path;

            const char *op_type = cJSON_IsTrue(use_regex) ? "(regex)" : "(string)";
            snprintf(details, sizeof(details), "%s %s", filename, op_type);
        }
    } else if (strcmp(tool_name, "Glob") == 0) {
        cJSON *pattern = cJSON_GetObjectItem(arguments, "pattern");
        if (cJSON_IsString(pattern)) {
            strncpy(details, pattern->valuestring, sizeof(details) - 1);
            details[sizeof(details) - 1] = '\0';
        }
    } else if (strcmp(tool_name, "Grep") == 0) {
        cJSON *pattern = cJSON_GetObjectItem(arguments, "pattern");
        cJSON *path = cJSON_GetObjectItem(arguments, "path");

        if (cJSON_IsString(pattern)) {
            if (cJSON_IsString(path) && strlen(path->valuestring) > 0 &&
                strcmp(path->valuestring, ".") != 0) {
                snprintf(details, sizeof(details), "\"%s\" in %s",
                        pattern->valuestring, path->valuestring);
            } else {
                snprintf(details, sizeof(details), "\"%s\"", pattern->valuestring);
            }
        }
    } else if (strcmp(tool_name, "TodoWrite") == 0) {
        cJSON *todos = cJSON_GetObjectItem(arguments, "todos");
        if (cJSON_IsArray(todos)) {
            int count = cJSON_GetArraySize(todos);
            snprintf(details, sizeof(details), "%d task%s", count, count == 1 ? "" : "s");
        }
    }

    return strlen(details) > 0 ? details : NULL;
}

static void print_error(const char *text) {
    // Log to file only (no stderr output)
    LOG_ERROR("%s", text);
}



// ============================================================================
// Data Structures
// ============================================================================

// Note: MessageRole, ContentType, ContentBlock, Message, and ConversationState
// are now defined in claude_internal.h for sharing across modules


// ============================================================================
// ESC Key Interrupt Handling
// ============================================================================

// Global interrupt flag - set to 1 when ESC is pressed
static volatile sig_atomic_t interrupt_requested = 0;



// Check for ESC key press without blocking
// Returns: 1 if ESC was pressed, 0 otherwise
int check_for_esc(void) {
    struct termios old_term, new_term;
    int esc_pressed = 0;

    // Save current terminal settings
    if (tcgetattr(STDIN_FILENO, &old_term) < 0) {
        return 0;  // Can't check, assume no ESC
    }

    // Set terminal to non-blocking mode
    new_term = old_term;
    new_term.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    new_term.c_cc[VMIN] = 0;   // Non-blocking
    new_term.c_cc[VTIME] = 0;  // No timeout
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

    // Check if there's input available
    fd_set readfds;
    struct timeval timeout;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;

    int ready = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
    if (ready > 0) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 27) {  // ESC key
                esc_pressed = 1;
                interrupt_requested = 1;

                // Drain any following characters (like [, etc. from arrow keys)
                while (read(STDIN_FILENO, &c, 1) == 1) {
                    // Keep draining
                }
            }
        }
    }

    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);

    return esc_pressed;
}


// ============================================================================
// Utility Functions
// ============================================================================

// For testing, we need to export some functions
#ifdef TEST_BUILD
#define STATIC
// Forward declarations for TEST_BUILD
char* read_file(const char *path);
int write_file(const char *path, const char *content);
char* resolve_path(const char *path, const char *working_dir);
cJSON* tool_read(cJSON *params, ConversationState *state);
cJSON* tool_write(cJSON *params, ConversationState *state);
cJSON* tool_edit(cJSON *params, ConversationState *state);
cJSON* tool_todo_write(cJSON *params, ConversationState *state);
static cJSON* tool_sleep(cJSON *params, ConversationState *state);
#else
#define STATIC static
// Forward declarations
char* read_file(const char *path);
int write_file(const char *path, const char *content);
char* resolve_path(const char *path, const char *working_dir);
#endif


char* read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = malloc((size_t)fsize + 1);
    if (content) {
        size_t bytes_read = fread(content, 1, (size_t)fsize, f);
        (void)bytes_read; // Suppress unused result warning
        content[fsize] = 0;
    }

    fclose(f);
    return content;
}

int write_file(const char *path, const char *content) {
    // Create parent directories if they don't exist
    char *path_copy = strdup(path);
    if (!path_copy) return -1;

    // Extract directory path
    char *dir_path = dirname(path_copy);

    // Create directory recursively (ignore errors if directory already exists)
    char mkdir_cmd[PATH_MAX];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s' 2>/dev/null", dir_path);
    int mkdir_result = system(mkdir_cmd);
    (void)mkdir_result; // Suppress unused result warning

    free(path_copy);

    // Now try to open/create the file
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    return (written == len) ? 0 : -1;
}

char* resolve_path(const char *path, const char *working_dir) {
    char *resolved = malloc(PATH_MAX);
    if (!resolved) return NULL;

    if (path[0] == '/') {
        snprintf(resolved, PATH_MAX, "%s", path);
    } else {
        snprintf(resolved, PATH_MAX, "%s/%s", working_dir, path);
    }

    // Try realpath first (works if file exists)
    char *clean = realpath(resolved, NULL);
    if (clean) {
        free(resolved);
        return clean;
    }

    // If realpath failed, the file might not exist yet (e.g., Write tool)
    // Resolve the parent directory and append the filename
    char *last_slash = strrchr(resolved, '/');
    if (!last_slash) {
        // No slash found - shouldn't happen since we added working_dir
        free(resolved);
        return NULL;
    }

    // Split into directory and filename
    *last_slash = '\0';
    char *filename = last_slash + 1;

    // Resolve parent directory
    char *clean_dir = realpath(resolved, NULL);
    if (!clean_dir) {
        // Parent directory doesn't exist either
        free(resolved);
        return NULL;
    }

    // Combine resolved directory with filename
    char *result = malloc(PATH_MAX);
    if (!result) {
        free(clean_dir);
        free(resolved);
        return NULL;
    }
    snprintf(result, PATH_MAX, "%s/%s", clean_dir, filename);

    free(clean_dir);
    free(resolved);
    return result;
}

// Add a directory to the additional working directories list
// Returns: 0 on success, -1 on error
int add_directory(ConversationState *state, const char *path) {
    if (!state || !path) {
        return -1;
    }

    if (conversation_state_lock(state) != 0) {
        return -1;
    }

    // Validate that directory exists
    int result = -1;
    struct stat st;
    char *resolved_path = NULL;

    // Resolve path (handle relative paths)
    if (path[0] == '/') {
        resolved_path = realpath(path, NULL);
    } else {
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", state->working_dir, path);
        resolved_path = realpath(full_path, NULL);
    }

    if (!resolved_path) {
        goto out;  // Path doesn't exist or can't be resolved
    }

    if (stat(resolved_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        free(resolved_path);
        goto out;  // Not a directory
    }

    // Check if directory is already in the list (avoid duplicates)
    if (strcmp(resolved_path, state->working_dir) == 0) {
        free(resolved_path);
        goto out;  // Already the main working directory
    }

    for (int i = 0; i < state->additional_dirs_count; i++) {
        if (strcmp(resolved_path, state->additional_dirs[i]) == 0) {
            free(resolved_path);
            goto out;  // Already in additional directories
        }
    }

    // Expand array if needed
    if (state->additional_dirs_count >= state->additional_dirs_capacity) {
        int new_capacity = state->additional_dirs_capacity == 0 ? 4 : state->additional_dirs_capacity * 2;
        char **new_array = realloc(state->additional_dirs, (size_t)new_capacity * sizeof(char*));
        if (!new_array) {
            free(resolved_path);
            goto out;  // Out of memory
        }
        state->additional_dirs = new_array;
        state->additional_dirs_capacity = new_capacity;
    }

    // Add directory to list
    state->additional_dirs[state->additional_dirs_count++] = resolved_path;
    resolved_path = NULL;
    result = 0;

out:
    conversation_state_unlock(state);
    free(resolved_path);
    return result;
}

// ============================================================================
// Diff Functionality
// ============================================================================

// Show unified diff between original content and current file
// Returns 0 on success, -1 on error
static int show_diff(const char *file_path, const char *original_content) {
    // Create temporary file for original content
    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.claude_diff.XXXXXX", file_path);

    int fd = mkstemp(temp_path);
    if (fd == -1) {
        LOG_ERROR("Failed to create temporary file for diff");
        return -1;
    }

    // Write original content to temp file
    size_t content_len = strlen(original_content);
    ssize_t written = write(fd, original_content, content_len);
    close(fd);

    if (written < 0 || (size_t)written != content_len) {
        LOG_ERROR("Failed to write original content to temp file");
        unlink(temp_path);
        return -1;
    }

    // Run diff command to show changes
    char diff_cmd[PATH_MAX * 2];
    snprintf(diff_cmd, sizeof(diff_cmd), "diff -u \"%s\" \"%s\"", temp_path, file_path);

    FILE *pipe = popen(diff_cmd, "r");
    if (!pipe) {
        LOG_ERROR("Failed to run diff command");
        unlink(temp_path);
        return -1;
    }

    // Get color codes for diff elements
    char add_color[32], remove_color[32], header_color[32], context_color[32];
    const char *add_color_start, *remove_color_start, *header_color_start, *context_color_start;

    // Try to get colors from colorscheme, fall back to ANSI colors
    if (get_colorscheme_color(COLORSCHEME_DIFF_ADD, add_color, sizeof(add_color)) == 0) {
        add_color_start = add_color;
    } else {
        LOG_WARN("Using fallback ANSI color for DIFF_ADD");
        add_color_start = ANSI_FALLBACK_DIFF_ADD;
    }

    if (get_colorscheme_color(COLORSCHEME_DIFF_REMOVE, remove_color, sizeof(remove_color)) == 0) {
        remove_color_start = remove_color;
    } else {
        LOG_WARN("Using fallback ANSI color for DIFF_REMOVE");
        remove_color_start = ANSI_FALLBACK_DIFF_REMOVE;
    }

    if (get_colorscheme_color(COLORSCHEME_DIFF_HEADER, header_color, sizeof(header_color)) == 0) {
        header_color_start = header_color;
    } else {
        LOG_WARN("Using fallback ANSI color for DIFF_HEADER");
        header_color_start = ANSI_FALLBACK_DIFF_HEADER;
    }

    if (get_colorscheme_color(COLORSCHEME_DIFF_CONTEXT, context_color, sizeof(context_color)) == 0) {
        context_color_start = context_color;
    } else {
        LOG_WARN("Using fallback ANSI color for DIFF_CONTEXT");
        context_color_start = ANSI_FALLBACK_DIFF_CONTEXT;
    }

    // Read and display colorized diff output

    char line[1024];
    int has_diff = 0;

    while (fgets(line, sizeof(line), pipe)) {
        has_diff = 1;

        // Colorize based on line prefix
        if (line[0] == '+' && line[1] != '+') {
            // Added line (but not +++ header)
            printf("%s%s%s", add_color_start, line, ANSI_RESET);
        } else if (line[0] == '-' && line[1] != '-') {
            // Removed line (but not --- header)
            printf("%s%s%s", remove_color_start, line, ANSI_RESET);
        } else if (strncmp(line, "@@", 2) == 0) {
            // Line number context (@@ -line,count +line,count @@)
            printf("%s%s%s", context_color_start, line, ANSI_RESET);
        } else if (strncmp(line, "---", 3) == 0 || strncmp(line, "+++", 3) == 0) {
            // File headers
            printf("%s%s%s", header_color_start, line, ANSI_RESET);
        } else {
            // Context lines (no change)
            printf("%s", line);
        }
    }

    int result = pclose(pipe);
    unlink(temp_path);

    if (!has_diff) {
        printf("(No changes - files are identical)\n");
    } else if (result == 0) {
        // diff exit code 0 means no differences found
        printf("(No differences found)\n");
    }


    return 0;
}

// ============================================================================
// Tool Implementations
// ============================================================================

static cJSON* tool_bash(cJSON *params, ConversationState *state) {
    (void)state;  // Unused parameter

    const cJSON *cmd_json = cJSON_GetObjectItem(params, "command");
    if (!cmd_json || !cJSON_IsString(cmd_json)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Missing 'command' parameter");
        return error;
    }

    const char *command = cmd_json->valuestring;

    // Execute command and capture output
    char buffer[BUFFER_SIZE];
    FILE *pipe = popen(command, "r");
    if (!pipe) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Failed to execute command");
        return error;
    }

    char *output = NULL;
    size_t total_size = 0;

    while (fgets(buffer, sizeof(buffer), pipe)) {
        // Add cancellation point to allow thread cancellation during long reads
        pthread_testcancel();

        size_t len = strlen(buffer);
        char *new_output = realloc(output, total_size + len + 1);
        if (!new_output) {
            free(output);
            pclose(pipe);
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "Out of memory");
            return error;
        }
        output = new_output;
        memcpy(output + total_size, buffer, len);
        total_size += len;
        output[total_size] = '\0';
    }

    int status = pclose(pipe);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "exit_code", exit_code);
    cJSON_AddStringToObject(result, "output", output ? output : "");

    free(output);
    return result;
}

STATIC cJSON* tool_read(cJSON *params, ConversationState *state) {
    const cJSON *path_json = cJSON_GetObjectItem(params, "file_path");
    if (!path_json || !cJSON_IsString(path_json)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Missing 'file_path' parameter");
        return error;
    }

    // Get optional line range parameters
    const cJSON *start_line_json = cJSON_GetObjectItem(params, "start_line");
    const cJSON *end_line_json = cJSON_GetObjectItem(params, "end_line");

    int start_line = -1;  // -1 means no limit
    int end_line = -1;    // -1 means no limit

    if (start_line_json && cJSON_IsNumber(start_line_json)) {
        start_line = start_line_json->valueint;
        if (start_line < 1) {
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "start_line must be >= 1");
            return error;
        }
    }

    if (end_line_json && cJSON_IsNumber(end_line_json)) {
        end_line = end_line_json->valueint;
        if (end_line < 1) {
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "end_line must be >= 1");
            return error;
        }
    }

    // Validate line range
    if (start_line > 0 && end_line > 0 && start_line > end_line) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "start_line must be <= end_line");
        return error;
    }

    char *resolved_path = resolve_path(path_json->valuestring, state->working_dir);
    if (!resolved_path) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Failed to resolve path");
        return error;
    }

    char *content = read_file(resolved_path);
    free(resolved_path);

    if (!content) {
        cJSON *error = cJSON_CreateObject();
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Failed to read file: %s", strerror(errno));
        cJSON_AddStringToObject(error, "error", err_msg);
        return error;
    }

    // If line range is specified, extract only those lines
    char *filtered_content = content;
    int total_lines = 0;

    if (start_line > 0 || end_line > 0) {
        // Count total lines and build filtered content
        char *result_buffer = NULL;
        size_t result_size = 0;
        int current_line = 1;
        char *line_start = content;
        char *pos = content;

        while (*pos) {
            if (*pos == '\n') {
                // Found end of line
                int line_len = (int)(pos - line_start + 1);  // Include the newline

                // Check if this line should be included
                int include = 1;
                if (start_line > 0 && current_line < start_line) include = 0;
                if (end_line > 0 && current_line > end_line) include = 0;

                if (include) {
                    // Add this line to result
                    char *new_buffer = realloc(result_buffer, result_size + (size_t)line_len + 1);
                    if (!new_buffer) {
                        free(result_buffer);
                        free(content);
                        cJSON *error = cJSON_CreateObject();
                        cJSON_AddStringToObject(error, "error", "Out of memory");
                        return error;
                    }
                    result_buffer = new_buffer;
                    memcpy(result_buffer + result_size, line_start, (size_t)line_len);
                    result_size += (size_t)line_len;
                    result_buffer[result_size] = '\0';
                }

                current_line++;
                line_start = pos + 1;

                // Stop if we've reached end_line
                if (end_line > 0 && current_line > end_line) {
                    break;
                }
            }
            pos++;
        }

        // Handle last line (if file doesn't end with newline)
        if (*line_start && (end_line < 0 || current_line <= end_line) &&
            (start_line < 0 || current_line >= start_line)) {
            int line_len = (int)strlen(line_start);
            char *new_buffer = realloc(result_buffer, result_size + (size_t)line_len + 1);
            if (!new_buffer) {
                free(result_buffer);
                free(content);
                cJSON *error = cJSON_CreateObject();
                cJSON_AddStringToObject(error, "error", "Out of memory");
                return error;
            }
            result_buffer = new_buffer;
            memcpy(result_buffer + result_size, line_start, (size_t)line_len);
            result_size += (size_t)line_len;
            result_buffer[result_size] = '\0';
            current_line++;
        }

        total_lines = current_line - 1;

        if (!result_buffer) {
            result_buffer = strdup("");
        }

        free(content);
        filtered_content = result_buffer;
    } else {
        // Count total lines for the full file
        char *pos = content;
        total_lines = 0;
        while (*pos) {
            if (*pos == '\n') total_lines++;
            pos++;
        }
        if (pos > content && *(pos-1) != '\n') total_lines++;  // Last line without newline
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "content", filtered_content);
    cJSON_AddNumberToObject(result, "total_lines", total_lines);

    if (start_line > 0 || end_line > 0) {
        cJSON_AddNumberToObject(result, "start_line", start_line > 0 ? start_line : 1);
        cJSON_AddNumberToObject(result, "end_line", end_line > 0 ? end_line : total_lines);
    }

    free(filtered_content);

    return result;
}

STATIC cJSON* tool_write(cJSON *params, ConversationState *state) {
    const cJSON *path_json = cJSON_GetObjectItem(params, "file_path");
    const cJSON *content_json = cJSON_GetObjectItem(params, "content");

    if (!path_json || !cJSON_IsString(path_json)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Missing 'file_path' parameter");
        return error;
    }

    if (!content_json || !cJSON_IsString(content_json)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Missing 'content' parameter");
        return error;
    }

    // Check if content is in patch format
    const char *content = content_json->valuestring;
    if (is_patch_format(content)) {
        LOG_INFO("Detected patch format in Write tool, parsing and applying...");

        // Parse the patch
        ParsedPatch *patch = parse_patch_format(content);
        if (!patch) {
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "Failed to parse patch format");
            return error;
        }

        // Apply the patch
        cJSON *result = apply_patch(patch, state);
        free_parsed_patch(patch);

        return result;
    }

    char *resolved_path = resolve_path(path_json->valuestring, state->working_dir);
    if (!resolved_path) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Failed to resolve path");
        return error;
    }

    // Check if file exists and read original content for diff
    char *original_content = NULL;
    FILE *existing_file = fopen(resolved_path, "r");
    if (existing_file) {
        fclose(existing_file);
        original_content = read_file(resolved_path);
        if (!original_content) {
            free(resolved_path);
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "Failed to read existing file for diff comparison");
            return error;
        }
    }

    int ret = write_file(resolved_path, content_json->valuestring);

    // Show diff if write was successful and file existed before
    if (ret == 0) {
        if (original_content) {
            show_diff(resolved_path, original_content);
        } else {
            // New file creation
            printf("\n--- Created new file: %s ---\n", resolved_path);
            printf("(New file written - no previous content to compare)\n\n");
        }
    }

    free(resolved_path);
    free(original_content);

    if (ret != 0) {
        cJSON *error = cJSON_CreateObject();
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Failed to write file: %s", strerror(errno));
        cJSON_AddStringToObject(error, "error", err_msg);
        return error;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "success");

    return result;
}

// Helper function for simple string multi-replace

// ============================================================================
// Parallel tool execution support
// ============================================================================

// Forward declaration
static cJSON* execute_tool(const char *tool_name, cJSON *input, ConversationState *state);

typedef struct {
    char *tool_use_id;            // duplicated tool_call id
    const char *tool_name;        // name of the tool
    cJSON *input;                 // arguments for tool
    ConversationState *state;     // conversation state
    InternalContent *result_block;   // pointer to results array slot
} ToolThreadArg;

// Cleanup handler for tool thread cancellation
static void tool_thread_cleanup(void *arg) {
    ToolThreadArg *t = (ToolThreadArg *)arg;
    // Free input JSON if not already freed
    if (t->input) {
        cJSON_Delete(t->input);
        t->input = NULL;
    }
    // Mark result as cancelled
    t->result_block->type = INTERNAL_TOOL_RESPONSE;
    t->result_block->tool_id = t->tool_use_id;
    t->result_block->tool_name = strdup(t->tool_name);
    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "Tool execution cancelled by user");
    t->result_block->tool_output = error;
    t->result_block->is_error = 1;
}

static void *tool_thread_func(void *arg) {
    ToolThreadArg *t = (ToolThreadArg *)arg;

    // Enable thread cancellation
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    // Register cleanup handler
    pthread_cleanup_push(tool_thread_cleanup, arg);

    // Execute the tool
    cJSON *res = execute_tool(t->tool_name, t->input, t->state);
    // Free input JSON
    cJSON_Delete(t->input);
    t->input = NULL;  // Mark as freed for cleanup handler
    // Populate result block
    t->result_block->type = INTERNAL_TOOL_RESPONSE;
    t->result_block->tool_id = t->tool_use_id;
    t->result_block->tool_name = strdup(t->tool_name);  // Store tool name for error reporting
    t->result_block->tool_output = res;
    t->result_block->is_error = cJSON_HasObjectItem(res, "error");

    // Pop cleanup handler (execute=0 means don't run it on normal exit)
    pthread_cleanup_pop(0);

    return NULL;
}

// Helper function for simple string multi-replace
static char* str_replace_all(const char *content, const char *old_str, const char *new_str, int *replace_count) {
    *replace_count = 0;

    // Count occurrences
    const char *pos = content;
    while ((pos = strstr(pos, old_str)) != NULL) {
        (*replace_count)++;
        pos += strlen(old_str);
    }

    if (*replace_count == 0) {
        return NULL;
    }

    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t content_len = strlen(content);
    size_t result_len = content_len + (size_t)(*replace_count) * (new_len - old_len);

    char *result = malloc(result_len + 1);
    if (!result) return NULL;

    char *dest = result;
    const char *src = content;

    while ((pos = strstr(src, old_str)) != NULL) {
        size_t len = (size_t)(pos - src);
        memcpy(dest, src, len);
        dest += len;
        memcpy(dest, new_str, new_len);
        dest += new_len;
        src = pos + old_len;
    }

    strcpy(dest, src);
    return result;
}

// Helper function for regex replacement
static char* regex_replace(const char *content, const char *pattern, const char *replacement,
                          int replace_all, int *replace_count, char **error_msg) {
    *replace_count = 0;

    regex_t regex;
    int ret = regcomp(&regex, pattern, REG_EXTENDED);
    if (ret != 0) {
        char err_buf[256];
        regerror(ret, &regex, err_buf, sizeof(err_buf));
        *error_msg = strdup(err_buf);
        return NULL;
    }

    regmatch_t match;
    const char *src = content;
    size_t result_capacity = strlen(content) * 2;
    char *result = malloc(result_capacity);
    if (!result) {
        regfree(&regex);
        *error_msg = strdup("Out of memory");
        return NULL;
    }

    char *dest = result;
    size_t dest_len = 0;
    size_t repl_len = strlen(replacement);

    while (regexec(&regex, src, 1, &match, 0) == 0) {
        (*replace_count)++;

        // Copy text before match
        size_t prefix_len = (size_t)match.rm_so;
        if (dest_len + prefix_len + repl_len >= result_capacity) {
            result_capacity *= 2;
            char *new_result = realloc(result, result_capacity);
            if (!new_result) {
                free(result);
                regfree(&regex);
                *error_msg = strdup("Out of memory");
                return NULL;
            }
            result = new_result;
            dest = result + dest_len;
        }

        memcpy(dest, src, prefix_len);
        dest += prefix_len;
        dest_len += prefix_len;

        // Copy replacement
        memcpy(dest, replacement, repl_len);
        dest += repl_len;
        dest_len += repl_len;

        src += match.rm_eo;

        if (!replace_all) break;
    }

    // Copy remaining text
    size_t remaining = strlen(src);
    if (dest_len + remaining >= result_capacity) {
        char *new_result = realloc(result, dest_len + remaining + 1);
        if (!new_result) {
            free(result);
            regfree(&regex);
            *error_msg = strdup("Out of memory");
            return NULL;
        }
        result = new_result;
        dest = result + dest_len;
    }

    strcpy(dest, src);
    regfree(&regex);

    if (*replace_count == 0) {
        free(result);
        return NULL;
    }

    return result;
}

STATIC cJSON* tool_edit(cJSON *params, ConversationState *state) {
    const cJSON *path_json = cJSON_GetObjectItem(params, "file_path");
    const cJSON *old_json = cJSON_GetObjectItem(params, "old_string");
    const cJSON *new_json = cJSON_GetObjectItem(params, "new_string");
    const cJSON *replace_all_json = cJSON_GetObjectItem(params, "replace_all");
    const cJSON *use_regex_json = cJSON_GetObjectItem(params, "use_regex");

    if (!path_json || !old_json || !new_json) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Missing required parameters");
        return error;
    }

    // Check if new_string content is in patch format
    const char *new_string_content = new_json->valuestring;
    if (is_patch_format(new_string_content)) {
        LOG_INFO("Detected patch format in Edit tool, parsing and applying...");

        // Parse the patch
        ParsedPatch *patch = parse_patch_format(new_string_content);
        if (!patch) {
            cJSON *error = cJSON_CreateObject();
            cJSON_AddStringToObject(error, "error", "Failed to parse patch format");
            return error;
        }

        // Apply the patch
        cJSON *result = apply_patch(patch, state);
        free_parsed_patch(patch);

        return result;
    }

    int replace_all = replace_all_json && cJSON_IsBool(replace_all_json) ?
                      cJSON_IsTrue(replace_all_json) : 0;
    int use_regex = use_regex_json && cJSON_IsBool(use_regex_json) ?
                    cJSON_IsTrue(use_regex_json) : 0;

    char *resolved_path = resolve_path(path_json->valuestring, state->working_dir);
    if (!resolved_path) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Failed to resolve path");
        return error;
    }

    char *content = read_file(resolved_path);
    if (!content) {
        free(resolved_path);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Failed to read file");
        return error;
    }

    // Save original content for diff comparison
    char *original_content = strdup(content);
    if (!original_content) {
        free(content);
        free(resolved_path);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Failed to allocate memory for diff");
        return error;
    }

    const char *old_str = old_json->valuestring;
    const char *new_str = new_json->valuestring;
    char *new_content = NULL;
    int replace_count = 0;
    char *error_msg = NULL;

    if (use_regex) {
        // Regex-based replacement
        new_content = regex_replace(content, old_str, new_str, replace_all, &replace_count, &error_msg);
    } else if (replace_all) {
        // Simple string multi-replace
        new_content = str_replace_all(content, old_str, new_str, &replace_count);
    } else {
        // Simple string single replace (original behavior)
        char *pos = strstr(content, old_str);
        if (pos) {
            replace_count = 1;
            size_t old_len = strlen(old_str);
            size_t new_len = strlen(new_str);
            size_t content_len = strlen(content);
            size_t offset = (size_t)(pos - content);

            new_content = malloc(content_len - old_len + new_len + 1);
            if (new_content) {
                memcpy(new_content, content, offset);
                memcpy(new_content + offset, new_str, new_len);
                memcpy(new_content + offset + new_len, content + offset + old_len,
                       content_len - offset - old_len + 1);
            }
        }
    }

    if (!new_content) {
        free(content);
        free(original_content);
        free(resolved_path);
        cJSON *error = cJSON_CreateObject();
        if (error_msg) {
            cJSON_AddStringToObject(error, "error", error_msg);
            free(error_msg);
        } else if (replace_count == 0) {
            cJSON_AddStringToObject(error, "error",
                use_regex ? "Pattern not found in file" : "String not found in file");
        } else {
            cJSON_AddStringToObject(error, "error", "Out of memory");
        }
        return error;
    }

    int ret = write_file(resolved_path, new_content);

    // Show diff if edit was successful
    if (ret == 0) {
        show_diff(resolved_path, original_content);
    }

    free(content);
    free(new_content);
    free(resolved_path);
    free(original_content);

    if (ret != 0) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Failed to write file");
        return error;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "success");
    cJSON_AddNumberToObject(result, "replacements", replace_count);
    return result;
}

static cJSON* tool_glob(cJSON *params, ConversationState *state) {
    const cJSON *pattern_json = cJSON_GetObjectItem(params, "pattern");
    if (!pattern_json || !cJSON_IsString(pattern_json)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Missing 'pattern' parameter");
        return error;
    }

    const char *pattern = pattern_json->valuestring;
    cJSON *result = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();
    int total_count = 0;

    // Search in main working directory
    char full_pattern[PATH_MAX];
    snprintf(full_pattern, sizeof(full_pattern), "%s/%s", state->working_dir, pattern);

    glob_t glob_result;
    int ret = glob(full_pattern, GLOB_TILDE, NULL, &glob_result);

    if (ret == 0) {
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
            cJSON_AddItemToArray(files, cJSON_CreateString(glob_result.gl_pathv[i]));
            total_count++;
        }
        globfree(&glob_result);
    }

    // Search in additional working directories
    for (int dir_idx = 0; dir_idx < state->additional_dirs_count; dir_idx++) {
        snprintf(full_pattern, sizeof(full_pattern), "%s/%s",
                 state->additional_dirs[dir_idx], pattern);

        ret = glob(full_pattern, GLOB_TILDE, NULL, &glob_result);

        if (ret == 0) {
            for (size_t i = 0; i < glob_result.gl_pathc; i++) {
                cJSON_AddItemToArray(files, cJSON_CreateString(glob_result.gl_pathv[i]));
                total_count++;
            }
            globfree(&glob_result);
        }
    }

    cJSON_AddItemToObject(result, "files", files);
    cJSON_AddNumberToObject(result, "count", total_count);

    return result;
}

static cJSON* tool_grep(cJSON *params, ConversationState *state) {
    const cJSON *pattern_json = cJSON_GetObjectItem(params, "pattern");
    const cJSON *path_json = cJSON_GetObjectItem(params, "path");

    if (!pattern_json || !cJSON_IsString(pattern_json)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Missing 'pattern' parameter");
        return error;
    }

    const char *pattern = pattern_json->valuestring;
    const char *path = path_json && cJSON_IsString(path_json) ?
                       path_json->valuestring : ".";

    cJSON *result = cJSON_CreateObject();
    cJSON *matches = cJSON_CreateArray();

    // Search in main working directory
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command),
             "cd %s && grep -r -n '%s' %s 2>/dev/null || true",
             state->working_dir, pattern, path);

    FILE *pipe = popen(command, "r");
    if (!pipe) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Failed to execute grep");
        return error;
    }

    char buffer[BUFFER_SIZE];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        buffer[strcspn(buffer, "\n")] = 0;  // Remove newline
        cJSON_AddItemToArray(matches, cJSON_CreateString(buffer));
    }
    pclose(pipe);

    // Search in additional working directories
    for (int dir_idx = 0; dir_idx < state->additional_dirs_count; dir_idx++) {
        snprintf(command, sizeof(command),
                 "cd %s && grep -r -n '%s' %s 2>/dev/null || true",
                 state->additional_dirs[dir_idx], pattern, path);

        pipe = popen(command, "r");
        if (!pipe) continue;  // Skip this directory on error

        while (fgets(buffer, sizeof(buffer), pipe)) {
            buffer[strcspn(buffer, "\n")] = 0;  // Remove newline
            cJSON_AddItemToArray(matches, cJSON_CreateString(buffer));
        }
        pclose(pipe);
    }

    cJSON_AddItemToObject(result, "matches", matches);
    return result;
}

STATIC cJSON* tool_todo_write(cJSON *params, ConversationState *state) {
    const cJSON *todos_json = cJSON_GetObjectItem(params, "todos");

    if (!todos_json || !cJSON_IsArray(todos_json)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Missing or invalid 'todos' parameter (must be array)");
        return error;
    }

    // Ensure todo_list is initialized
    if (!state->todo_list) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Todo list not initialized");
        return error;
    }

    // Clear existing todos
    todo_clear(state->todo_list);

    // Parse and add each todo
    int added = 0;
    int total = cJSON_GetArraySize(todos_json);

    for (int i = 0; i < total; i++) {
        cJSON *todo_item = cJSON_GetArrayItem(todos_json, i);
        if (!cJSON_IsObject(todo_item)) continue;

        const cJSON *content_json = cJSON_GetObjectItem(todo_item, "content");
        const cJSON *active_form_json = cJSON_GetObjectItem(todo_item, "activeForm");
        const cJSON *status_json = cJSON_GetObjectItem(todo_item, "status");

        if (!content_json || !cJSON_IsString(content_json) ||
            !active_form_json || !cJSON_IsString(active_form_json) ||
            !status_json || !cJSON_IsString(status_json)) {
            continue;  // Skip invalid todo items
        }

        const char *content = content_json->valuestring;
        const char *active_form = active_form_json->valuestring;
        const char *status_str = status_json->valuestring;

        // Parse status string to TodoStatus enum
        TodoStatus status;
        if (strcmp(status_str, "completed") == 0) {
            status = TODO_COMPLETED;
        } else if (strcmp(status_str, "in_progress") == 0) {
            status = TODO_IN_PROGRESS;
        } else if (strcmp(status_str, "pending") == 0) {
            status = TODO_PENDING;
        } else {
            continue;  // Invalid status, skip this item
        }

        // Add the todo item
        if (todo_add(state->todo_list, content, active_form, status) == 0) {
            added++;
        }
    }

    // Render the updated todo list to terminal
    if (added > 0) {
        todo_render(state->todo_list);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "success");
    cJSON_AddNumberToObject(result, "added", added);
    cJSON_AddNumberToObject(result, "total", total);

    return result;
}


// ============================================================================
// Sleep Tool Implementation
// ============================================================================

/**
 * tool_sleep - pauses execution for specified duration
 * params: { "duration": integer (seconds) }
 */
STATIC cJSON* tool_sleep(cJSON *params, ConversationState *state) {
    (void)state;
    cJSON *duration_json = cJSON_GetObjectItem(params, "duration");
    if (!duration_json || !cJSON_IsNumber(duration_json)) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Missing or invalid 'duration' parameter (must be number of seconds)");
        return error;
    }
    int duration = duration_json->valueint;
    if (duration < 0) duration = 0;
    struct timespec req = { .tv_sec = duration, .tv_nsec = 0 };
    // Sleep for the duration (seconds)
    nanosleep(&req, NULL);
    // Return success result
    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "success");
    cJSON_AddNumberToObject(result, "duration", duration);
    return result;
}

// ============================================================================
// Tool Registry
// ============================================================================

typedef struct {
    const char *name;
    cJSON* (*handler)(cJSON *params, ConversationState *state);
} Tool;

static Tool tools[] = {
    {"Sleep", tool_sleep},
    {"Bash", tool_bash},
    {"Read", tool_read},
    {"Write", tool_write},
    {"Edit", tool_edit},
    {"Glob", tool_glob},
    {"Grep", tool_grep},
    {"TodoWrite", tool_todo_write},
};

static const int num_tools = sizeof(tools) / sizeof(Tool);

// Thread monitor for ESC checking during tool execution
typedef struct {
    pthread_t *threads;
    int thread_count;
    volatile int *done_flag;
} MonitorArg;

static void *tool_monitor_func(void *arg) {
    MonitorArg *ma = (MonitorArg *)arg;
    for (int i = 0; i < ma->thread_count; i++) {
        pthread_join(ma->threads[i], NULL);
    }
    *ma->done_flag = 1;
    return NULL;
}

static cJSON* execute_tool(const char *tool_name, cJSON *input, ConversationState *state) {
    // Time the tool execution
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    cJSON *result = NULL;
    for (int i = 0; i < num_tools; i++) {
        if (strcmp(tools[i].name, tool_name) == 0) {
            result = tools[i].handler(input, state);
            break;
        }
    }

    if (!result) {
        result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "error", "Unknown tool");
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    long duration_ms = (end.tv_sec - start.tv_sec) * 1000 +
                       (end.tv_nsec - start.tv_nsec) / 1000000;

    LOG_INFO("Tool '%s' executed in %ld ms", tool_name, duration_ms);

    return result;
}

// ============================================================================
// Tool Definitions for API
// ============================================================================

// Forward declaration for cache_control helper


cJSON* get_tool_definitions(int enable_caching) {
    cJSON *tool_array = cJSON_CreateArray();
    // Sleep tool
    cJSON *sleep_tool = cJSON_CreateObject();
    cJSON_AddStringToObject(sleep_tool, "type", "function");
    cJSON *sleep_func = cJSON_CreateObject();
    cJSON_AddStringToObject(sleep_func, "name", "Sleep");
    cJSON_AddStringToObject(sleep_func, "description", "Pauses execution for specified duration (seconds)");
    cJSON *sleep_params = cJSON_CreateObject();
    cJSON_AddStringToObject(sleep_params, "type", "object");
    cJSON *sleep_props = cJSON_CreateObject();
    cJSON *duration_prop = cJSON_CreateObject();
    cJSON_AddStringToObject(duration_prop, "type", "integer");
    cJSON_AddStringToObject(duration_prop, "description", "Duration to sleep in seconds");
    cJSON_AddItemToObject(sleep_props, "duration", duration_prop);
    cJSON_AddItemToObject(sleep_params, "properties", sleep_props);
    cJSON *sleep_req = cJSON_CreateArray();
    cJSON_AddItemToArray(sleep_req, cJSON_CreateString("duration"));
    cJSON_AddItemToObject(sleep_params, "required", sleep_req);
    cJSON_AddItemToObject(sleep_func, "parameters", sleep_params);
    cJSON_AddItemToObject(sleep_tool, "function", sleep_func);
    if (enable_caching) {
        add_cache_control(sleep_tool);
    }
    cJSON_AddItemToArray(tool_array, sleep_tool);

    // Bash tool
    cJSON *bash = cJSON_CreateObject();
    cJSON_AddStringToObject(bash, "type", "function");
    cJSON *bash_func = cJSON_CreateObject();
    cJSON_AddStringToObject(bash_func, "name", "Bash");
    cJSON_AddStringToObject(bash_func, "description", "Executes bash commands");
    cJSON *bash_params = cJSON_CreateObject();
    cJSON_AddStringToObject(bash_params, "type", "object");
    cJSON *bash_props = cJSON_CreateObject();
    cJSON *bash_cmd = cJSON_CreateObject();
    cJSON_AddStringToObject(bash_cmd, "type", "string");
    cJSON_AddStringToObject(bash_cmd, "description", "The command to execute");
    cJSON_AddItemToObject(bash_props, "command", bash_cmd);
    cJSON_AddItemToObject(bash_params, "properties", bash_props);
    cJSON *bash_req = cJSON_CreateArray();
    cJSON_AddItemToArray(bash_req, cJSON_CreateString("command"));
    cJSON_AddItemToObject(bash_params, "required", bash_req);
    cJSON_AddItemToObject(bash_func, "parameters", bash_params);
    cJSON_AddItemToObject(bash, "function", bash_func);
    cJSON_AddItemToArray(tool_array, bash);

    // Read tool
    cJSON *read = cJSON_CreateObject();
    cJSON_AddStringToObject(read, "type", "function");
    cJSON *read_func = cJSON_CreateObject();
    cJSON_AddStringToObject(read_func, "name", "Read");
    cJSON_AddStringToObject(read_func, "description",
        "Reads a file from the filesystem with optional line range support");
    cJSON *read_params = cJSON_CreateObject();
    cJSON_AddStringToObject(read_params, "type", "object");
    cJSON *read_props = cJSON_CreateObject();
    cJSON *read_path = cJSON_CreateObject();
    cJSON_AddStringToObject(read_path, "type", "string");
    cJSON_AddStringToObject(read_path, "description", "The absolute path to the file");
    cJSON_AddItemToObject(read_props, "file_path", read_path);
    cJSON *read_start = cJSON_CreateObject();
    cJSON_AddStringToObject(read_start, "type", "integer");
    cJSON_AddStringToObject(read_start, "description",
        "Optional: Starting line number (1-indexed, inclusive)");
    cJSON_AddItemToObject(read_props, "start_line", read_start);
    cJSON *read_end = cJSON_CreateObject();
    cJSON_AddStringToObject(read_end, "type", "integer");
    cJSON_AddStringToObject(read_end, "description",
        "Optional: Ending line number (1-indexed, inclusive)");
    cJSON_AddItemToObject(read_props, "end_line", read_end);
    cJSON_AddItemToObject(read_params, "properties", read_props);
    cJSON *read_req = cJSON_CreateArray();
    cJSON_AddItemToArray(read_req, cJSON_CreateString("file_path"));
    cJSON_AddItemToObject(read_params, "required", read_req);
    cJSON_AddItemToObject(read_func, "parameters", read_params);
    cJSON_AddItemToObject(read, "function", read_func);
    cJSON_AddItemToArray(tool_array, read);

    // Write tool
    cJSON *write = cJSON_CreateObject();
    cJSON_AddStringToObject(write, "type", "function");
    cJSON *write_func = cJSON_CreateObject();
    cJSON_AddStringToObject(write_func, "name", "Write");
    cJSON_AddStringToObject(write_func, "description", "Writes content to a file");
    cJSON *write_params = cJSON_CreateObject();
    cJSON_AddStringToObject(write_params, "type", "object");
    cJSON *write_props = cJSON_CreateObject();
    cJSON *write_path = cJSON_CreateObject();
    cJSON_AddStringToObject(write_path, "type", "string");
    cJSON_AddItemToObject(write_props, "file_path", write_path);
    cJSON *write_content = cJSON_CreateObject();
    cJSON_AddStringToObject(write_content, "type", "string");
    cJSON_AddItemToObject(write_props, "content", write_content);
    cJSON_AddItemToObject(write_params, "properties", write_props);
    cJSON *write_req = cJSON_CreateArray();
    cJSON_AddItemToArray(write_req, cJSON_CreateString("file_path"));
    cJSON_AddItemToArray(write_req, cJSON_CreateString("content"));
    cJSON_AddItemToObject(write_params, "required", write_req);
    cJSON_AddItemToObject(write_func, "parameters", write_params);
    cJSON_AddItemToObject(write, "function", write_func);
    cJSON_AddItemToArray(tool_array, write);

    // Edit tool
    cJSON *edit = cJSON_CreateObject();
    cJSON_AddStringToObject(edit, "type", "function");
    cJSON *edit_func = cJSON_CreateObject();
    cJSON_AddStringToObject(edit_func, "name", "Edit");
    cJSON_AddStringToObject(edit_func, "description",
        "Performs string replacements in files with optional regex and multi-replace support");
    cJSON *edit_params = cJSON_CreateObject();
    cJSON_AddStringToObject(edit_params, "type", "object");
    cJSON *edit_props = cJSON_CreateObject();
    cJSON *edit_path = cJSON_CreateObject();
    cJSON_AddStringToObject(edit_path, "type", "string");
    cJSON_AddStringToObject(edit_path, "description", "Path to the file to edit");
    cJSON_AddItemToObject(edit_props, "file_path", edit_path);
    cJSON *old_str = cJSON_CreateObject();
    cJSON_AddStringToObject(old_str, "type", "string");
    cJSON_AddStringToObject(old_str, "description",
        "String or regex pattern to search for (use_regex must be true for regex)");
    cJSON_AddItemToObject(edit_props, "old_string", old_str);
    cJSON *new_str = cJSON_CreateObject();
    cJSON_AddStringToObject(new_str, "type", "string");
    cJSON_AddStringToObject(new_str, "description", "Replacement string");
    cJSON_AddItemToObject(edit_props, "new_string", new_str);
    cJSON *replace_all = cJSON_CreateObject();
    cJSON_AddStringToObject(replace_all, "type", "boolean");
    cJSON_AddStringToObject(replace_all, "description",
        "If true, replace all occurrences; if false, replace only first occurrence (default: false)");
    cJSON_AddItemToObject(edit_props, "replace_all", replace_all);
    cJSON *use_regex = cJSON_CreateObject();
    cJSON_AddStringToObject(use_regex, "type", "boolean");
    cJSON_AddStringToObject(use_regex, "description",
        "If true, treat old_string as POSIX extended regex pattern (default: false)");
    cJSON_AddItemToObject(edit_props, "use_regex", use_regex);
    cJSON_AddItemToObject(edit_params, "properties", edit_props);
    cJSON *edit_req = cJSON_CreateArray();
    cJSON_AddItemToArray(edit_req, cJSON_CreateString("file_path"));
    cJSON_AddItemToArray(edit_req, cJSON_CreateString("old_string"));
    cJSON_AddItemToArray(edit_req, cJSON_CreateString("new_string"));
    cJSON_AddItemToObject(edit_params, "required", edit_req);
    cJSON_AddItemToObject(edit_func, "parameters", edit_params);
    cJSON_AddItemToObject(edit, "function", edit_func);
    cJSON_AddItemToArray(tool_array, edit);

    // Glob tool
    cJSON *glob_tool = cJSON_CreateObject();
    cJSON_AddStringToObject(glob_tool, "type", "function");
    cJSON *glob_func = cJSON_CreateObject();
    cJSON_AddStringToObject(glob_func, "name", "Glob");
    cJSON_AddStringToObject(glob_func, "description", "Finds files matching a pattern");
    cJSON *glob_params = cJSON_CreateObject();
    cJSON_AddStringToObject(glob_params, "type", "object");
    cJSON *glob_props = cJSON_CreateObject();
    cJSON *glob_pattern = cJSON_CreateObject();
    cJSON_AddStringToObject(glob_pattern, "type", "string");
    cJSON_AddItemToObject(glob_props, "pattern", glob_pattern);
    cJSON_AddItemToObject(glob_params, "properties", glob_props);
    cJSON *glob_req = cJSON_CreateArray();
    cJSON_AddItemToArray(glob_req, cJSON_CreateString("pattern"));
    cJSON_AddItemToObject(glob_params, "required", glob_req);
    cJSON_AddItemToObject(glob_func, "parameters", glob_params);
    cJSON_AddItemToObject(glob_tool, "function", glob_func);
    cJSON_AddItemToArray(tool_array, glob_tool);

    // Grep tool
    cJSON *grep_tool = cJSON_CreateObject();
    cJSON_AddStringToObject(grep_tool, "type", "function");
    cJSON *grep_func = cJSON_CreateObject();
    cJSON_AddStringToObject(grep_func, "name", "Grep");
    cJSON_AddStringToObject(grep_func, "description", "Searches for patterns in files");
    cJSON *grep_params = cJSON_CreateObject();
    cJSON_AddStringToObject(grep_params, "type", "object");
    cJSON *grep_props = cJSON_CreateObject();
    cJSON *grep_pattern = cJSON_CreateObject();
    cJSON_AddStringToObject(grep_pattern, "type", "string");
    cJSON_AddItemToObject(grep_props, "pattern", grep_pattern);
    cJSON *grep_path = cJSON_CreateObject();
    cJSON_AddStringToObject(grep_path, "type", "string");
    cJSON_AddItemToObject(grep_props, "path", grep_path);
    cJSON_AddItemToObject(grep_params, "properties", grep_props);
    cJSON *grep_req = cJSON_CreateArray();
    cJSON_AddItemToArray(grep_req, cJSON_CreateString("pattern"));
    cJSON_AddItemToObject(grep_params, "required", grep_req);
    cJSON_AddItemToObject(grep_func, "parameters", grep_params);
    cJSON_AddItemToObject(grep_tool, "function", grep_func);
    cJSON_AddItemToArray(tool_array, grep_tool);

    // TodoWrite tool
    cJSON *todo_tool = cJSON_CreateObject();
    cJSON_AddStringToObject(todo_tool, "type", "function");
    cJSON *todo_func = cJSON_CreateObject();
    cJSON_AddStringToObject(todo_func, "name", "TodoWrite");
    cJSON_AddStringToObject(todo_func, "description",
        "Creates and updates a task list to track progress on multi-step tasks");
    cJSON *todo_params = cJSON_CreateObject();
    cJSON_AddStringToObject(todo_params, "type", "object");
    cJSON *todo_props = cJSON_CreateObject();

    // Define the todos array parameter
    cJSON *todos_array = cJSON_CreateObject();
    cJSON_AddStringToObject(todos_array, "type", "array");
    cJSON_AddStringToObject(todos_array, "description",
        "Array of todo items to display. Replaces the entire todo list.");

    // Define the items schema for the array
    cJSON *todos_items = cJSON_CreateObject();
    cJSON_AddStringToObject(todos_items, "type", "object");
    cJSON *item_props = cJSON_CreateObject();

    cJSON *content_prop = cJSON_CreateObject();
    cJSON_AddStringToObject(content_prop, "type", "string");
    cJSON_AddStringToObject(content_prop, "description",
        "Task description in imperative form (e.g., 'Run tests')");
    cJSON_AddItemToObject(item_props, "content", content_prop);

    cJSON *active_form_prop = cJSON_CreateObject();
    cJSON_AddStringToObject(active_form_prop, "type", "string");
    cJSON_AddStringToObject(active_form_prop, "description",
        "Task description in present continuous form (e.g., 'Running tests')");
    cJSON_AddItemToObject(item_props, "activeForm", active_form_prop);

    cJSON *status_prop = cJSON_CreateObject();
    cJSON_AddStringToObject(status_prop, "type", "string");
    cJSON *status_enum = cJSON_CreateArray();
    cJSON_AddItemToArray(status_enum, cJSON_CreateString("pending"));
    cJSON_AddItemToArray(status_enum, cJSON_CreateString("in_progress"));
    cJSON_AddItemToArray(status_enum, cJSON_CreateString("completed"));
    cJSON_AddItemToObject(status_prop, "enum", status_enum);
    cJSON_AddStringToObject(status_prop, "description",
        "Current status of the task");
    cJSON_AddItemToObject(item_props, "status", status_prop);

    cJSON_AddItemToObject(todos_items, "properties", item_props);
    cJSON *item_required = cJSON_CreateArray();
    cJSON_AddItemToArray(item_required, cJSON_CreateString("content"));
    cJSON_AddItemToArray(item_required, cJSON_CreateString("activeForm"));
    cJSON_AddItemToArray(item_required, cJSON_CreateString("status"));
    cJSON_AddItemToObject(todos_items, "required", item_required);

    cJSON_AddItemToObject(todos_array, "items", todos_items);
    cJSON_AddItemToObject(todo_props, "todos", todos_array);

    cJSON_AddItemToObject(todo_params, "properties", todo_props);
    cJSON *todo_req = cJSON_CreateArray();
    cJSON_AddItemToArray(todo_req, cJSON_CreateString("todos"));
    cJSON_AddItemToObject(todo_params, "required", todo_req);
    cJSON_AddItemToObject(todo_func, "parameters", todo_params);
    cJSON_AddItemToObject(todo_tool, "function", todo_func);

    // Add cache_control to the last tool (TodoWrite) if caching is enabled
    // This is the second cache breakpoint (tool definitions)
    if (enable_caching) {
        add_cache_control(todo_tool);
    }

    cJSON_AddItemToArray(tool_array, todo_tool);

    return tool_array;
}

// ============================================================================
// API Client
// ============================================================================

// Helper: Check if prompt caching is enabled
static int is_prompt_caching_enabled(void) {
    const char *disable_cache = getenv("DISABLE_PROMPT_CACHING");
    if (disable_cache && (strcmp(disable_cache, "1") == 0 ||
                          strcmp(disable_cache, "true") == 0 ||
                          strcmp(disable_cache, "TRUE") == 0)) {
        return 0;
    }
    return 1;
}

// Helper: Add cache_control to a JSON object (for content blocks)
void add_cache_control(cJSON *obj) {
    cJSON *cache_ctrl = cJSON_CreateObject();
    cJSON_AddStringToObject(cache_ctrl, "type", "ephemeral");
    cJSON_AddItemToObject(obj, "cache_control", cache_ctrl);
}

/**
 * Build request JSON from conversation state (in OpenAI format)
 * This is called by providers to get the request body
 * Returns: Newly allocated JSON string (caller must free), or NULL on error
 */
char* build_request_json_from_state(ConversationState *state) {
    if (!state) {
        LOG_ERROR("ConversationState is NULL");
        return NULL;
    }

    if (conversation_state_lock(state) != 0) {
        return NULL;
    }

    char *json_str = NULL;

    // Check if prompt caching is enabled
    int enable_caching = is_prompt_caching_enabled();
    LOG_DEBUG("Building request (caching: %s, messages: %d)",
              enable_caching ? "enabled" : "disabled", state->count);

    // Build request body
    cJSON *request = cJSON_CreateObject();
    if (!request) {
        LOG_ERROR("Failed to allocate request object");
        goto unlock;
    }

    cJSON_AddStringToObject(request, "model", state->model);
    cJSON_AddNumberToObject(request, "max_completion_tokens", MAX_TOKENS);

    // Add messages in OpenAI format
    cJSON *messages_array = cJSON_CreateArray();
    if (!messages_array) {
        LOG_ERROR("Failed to allocate messages array");
        goto unlock;
    }
    for (int i = 0; i < state->count; i++) {
        cJSON *msg = cJSON_CreateObject();
        if (!msg) {
            LOG_ERROR("Failed to allocate message object");
            goto unlock;
        }

        // Determine role
        const char *role;
        if (state->messages[i].role == MSG_SYSTEM) {
            role = "system";
        } else if (state->messages[i].role == MSG_USER) {
            role = "user";
        } else {
            role = "assistant";
        }
        cJSON_AddStringToObject(msg, "role", role);

        // Determine if this is one of the last 3 messages (for cache breakpoint)
        // We want to cache the last few messages to speed up subsequent turns
        int is_recent_message = (i >= state->count - 3) && enable_caching;

        // Build content based on message type
        if (state->messages[i].role == MSG_SYSTEM) {
            // System messages: use content array with cache_control if enabled
            if (state->messages[i].content_count > 0 &&
                state->messages[i].contents[0].type == INTERNAL_TEXT) {

                // For system messages, use content array to support cache_control
                cJSON *content_array = cJSON_CreateArray();
                cJSON *text_block = cJSON_CreateObject();
                cJSON_AddStringToObject(text_block, "type", "text");
                cJSON_AddStringToObject(text_block, "text", state->messages[i].contents[0].text);

                // Add cache_control to system message if caching is enabled
                // This is the first cache breakpoint (system prompt)
                if (enable_caching) {
                    add_cache_control(text_block);
                }

                cJSON_AddItemToArray(content_array, text_block);
                cJSON_AddItemToObject(msg, "content", content_array);
            }
        } else if (state->messages[i].role == MSG_USER) {
            // User messages: check if it's tool results or plain text
            int has_tool_results = 0;
            for (int j = 0; j < state->messages[i].content_count; j++) {
                if (state->messages[i].contents[j].type == INTERNAL_TOOL_RESPONSE) {
                    has_tool_results = 1;
                    break;
                }
            }

            if (has_tool_results) {
                // For tool results, we need to add them as "tool" role messages
                for (int j = 0; j < state->messages[i].content_count; j++) {
                    InternalContent *cb = &state->messages[i].contents[j];
                    if (cb->type == INTERNAL_TOOL_RESPONSE) {
                        cJSON *tool_msg = cJSON_CreateObject();
                        cJSON_AddStringToObject(tool_msg, "role", "tool");
                        cJSON_AddStringToObject(tool_msg, "tool_call_id", cb->tool_id);
                        // Convert result to string
                        char *result_str = cJSON_PrintUnformatted(cb->tool_output);
                        cJSON_AddStringToObject(tool_msg, "content", result_str);
                        free(result_str);
                        cJSON_AddItemToArray(messages_array, tool_msg);
                    }
                }
                // Free the msg object we created but won't use
                cJSON_Delete(msg);
                continue; // Skip adding the user message itself
            } else {
                // Regular user text message
                if (state->messages[i].content_count > 0 &&
                    state->messages[i].contents[0].type == INTERNAL_TEXT) {

                    // Use content array for recent messages to support cache_control
                    if (is_recent_message) {
                        cJSON *content_array = cJSON_CreateArray();
                        cJSON *text_block = cJSON_CreateObject();
                        cJSON_AddStringToObject(text_block, "type", "text");
                        cJSON_AddStringToObject(text_block, "text", state->messages[i].contents[0].text);

                        // Add cache_control to the last user message
                        if (i == state->count - 1) {
                            add_cache_control(text_block);
                        }

                        cJSON_AddItemToArray(content_array, text_block);
                        cJSON_AddItemToObject(msg, "content", content_array);
                    } else {
                        // For older messages, use simple string content
                        cJSON_AddStringToObject(msg, "content", state->messages[i].contents[0].text);
                    }
                }
            }
        } else {
            // Assistant messages
            cJSON *tool_calls = NULL;
            char *text_content = NULL;

            for (int j = 0; j < state->messages[i].content_count; j++) {
                InternalContent *cb = &state->messages[i].contents[j];

                if (cb->type == INTERNAL_TEXT) {
                    text_content = cb->text;
                } else if (cb->type == INTERNAL_TOOL_CALL) {
                    if (!tool_calls) {
                        tool_calls = cJSON_CreateArray();
                    }
                    cJSON *tool_call = cJSON_CreateObject();
                    cJSON_AddStringToObject(tool_call, "id", cb->tool_id);
                    cJSON_AddStringToObject(tool_call, "type", "function");
                    cJSON *function = cJSON_CreateObject();
                    cJSON_AddStringToObject(function, "name", cb->tool_name);
                    char *args_str = cJSON_PrintUnformatted(cb->tool_params);
                    cJSON_AddStringToObject(function, "arguments", args_str);
                    free(args_str);
                    cJSON_AddItemToObject(tool_call, "function", function);
                    cJSON_AddItemToArray(tool_calls, tool_call);
                }
            }

            // Add content (may be null if only tool calls)
            if (text_content) {
                cJSON_AddStringToObject(msg, "content", text_content);
            } else {
                cJSON_AddNullToObject(msg, "content");
            }

            if (tool_calls) {
                cJSON_AddItemToObject(msg, "tool_calls", tool_calls);
            }
        }

        cJSON_AddItemToArray(messages_array, msg);
    }

    cJSON_AddItemToObject(request, "messages", messages_array);

    // Add tools with cache_control support
    cJSON *tool_defs = get_tool_definitions(enable_caching);
    cJSON_AddItemToObject(request, "tools", tool_defs);

    conversation_state_unlock(state);
    state = NULL;

    json_str = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    LOG_DEBUG("Request built successfully (size: %zu bytes)", json_str ? strlen(json_str) : 0);
    return json_str;

unlock:
    conversation_state_unlock(state);
    if (request) {
        cJSON_Delete(request);
    }
    return NULL;
}

// ============================================================================
// API Response Management
// ============================================================================

/**
 * Free an ApiResponse structure and all its owned resources
 */
void api_response_free(ApiResponse *response) {
    if (!response) return;

    // Free assistant message text
    free(response->message.text);

    // Free tool calls
    if (response->tools) {
        for (int i = 0; i < response->tool_count; i++) {
            free(response->tools[i].id);
            free(response->tools[i].name);
            if (response->tools[i].parameters) {
                cJSON_Delete(response->tools[i].parameters);
            }
        }
        free(response->tools);
    }

    // Free raw response
    if (response->raw_response) {
        cJSON_Delete(response->raw_response);
    }

    free(response);
}

// ============================================================================
// API Call Logic
// ============================================================================

/**
 * Call API with retry logic (generic wrapper around provider->call_api)
 * Handles exponential backoff for retryable errors
 * Returns: ApiResponse or NULL on error
 */
static ApiResponse* call_api_with_retries(ConversationState *state) {
    if (!state || !state->provider) {
        LOG_ERROR("Invalid state or provider");
        return NULL;
    }

    int attempt_num = 1;
    int backoff_ms = INITIAL_BACKOFF_MS;

    struct timespec call_start, call_end, retry_start;
    clock_gettime(CLOCK_MONOTONIC, &call_start);
    retry_start = call_start;

    LOG_DEBUG("Starting API call (provider: %s, model: %s)",
              state->provider->name, state->model);

    while (1) {
        // Check if we've exceeded max retry duration
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - retry_start.tv_sec) * 1000 +
                         (now.tv_nsec - retry_start.tv_nsec) / 1000000;

        if (attempt_num > 1 && elapsed_ms >= state->max_retry_duration_ms) {
            LOG_ERROR("Maximum retry duration (%d ms) exceeded after %d attempts",
                     state->max_retry_duration_ms, attempt_num - 1);
            print_error("Maximum retry duration exceeded");
            return NULL;
        }

        // Call provider's single-attempt API call
        LOG_DEBUG("API call attempt %d (elapsed: %ld ms)", attempt_num, elapsed_ms);
        ApiCallResult result = state->provider->call_api(state->provider, state);

        // Success case
        if (result.response) {
            clock_gettime(CLOCK_MONOTONIC, &call_end);
            long total_ms = (call_end.tv_sec - call_start.tv_sec) * 1000 +
                           (call_end.tv_nsec - call_start.tv_nsec) / 1000000;

            LOG_INFO("API call succeeded (duration: %ld ms, provider duration: %ld ms, attempts: %d, auth_refreshed: %s)",
                     total_ms, result.duration_ms, attempt_num,
                     result.auth_refreshed ? "yes" : "no");

            // Log success to persistence
            if (state->persistence_db && result.raw_response) {
                // Tool count is already available in the ApiResponse
                int tool_count = result.response->tool_count;

                persistence_log_api_call(
                    state->persistence_db,
                    state->session_id,
                    state->api_url,
                    result.request_json ? result.request_json : "(request not available)",
                    result.raw_response,
                    state->model,
                    "success",
                    (int)result.http_status,
                    NULL,
                    result.duration_ms,
                    tool_count
                );
            }

            // Cleanup and return
            free(result.raw_response);
            free(result.request_json);
            free(result.error_message);
            return result.response;
        }

        // Error case - check if retryable
        LOG_WARN("API call failed (attempt %d): %s (HTTP %ld, retryable: %s)",
                 attempt_num,
                 result.error_message ? result.error_message : "(unknown)",
                 result.http_status,
                 result.is_retryable ? "yes" : "no");

        // Log error to persistence
        if (state->persistence_db) {
            persistence_log_api_call(
                state->persistence_db,
                state->session_id,
                state->api_url,
                result.request_json ? result.request_json : "(request not available)",
                result.raw_response,
                state->model,
                "error",
                (int)result.http_status,
                result.error_message,
                result.duration_ms,
                0
            );
        }

        // Check if we should retry
        if (!result.is_retryable) {
            // Non-retryable error
            char error_msg[512];
            snprintf(error_msg, sizeof(error_msg),
                    "API call failed: %s (HTTP %ld)",
                    result.error_message ? result.error_message : "unknown error",
                    result.http_status);
            print_error(error_msg);

            free(result.raw_response);
            free(result.request_json);
            free(result.error_message);
            return NULL;
        }

        // Calculate backoff with jitter (0-25% reduction)
        int jitter = rand() % (backoff_ms / 4);
        int delay_ms = backoff_ms - jitter;

        // Check if this delay would exceed max retry duration
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ms = (now.tv_sec - retry_start.tv_sec) * 1000 +
                    (now.tv_nsec - retry_start.tv_nsec) / 1000000;
        long remaining_ms = state->max_retry_duration_ms - elapsed_ms;

        if (delay_ms > remaining_ms) {
            delay_ms = (int)remaining_ms;
            if (delay_ms <= 0) {
                LOG_ERROR("Maximum retry duration (%d ms) exceeded", state->max_retry_duration_ms);
                print_error("Maximum retry duration exceeded");
                free(result.raw_response);
                free(result.request_json);
                free(result.error_message);
                return NULL;
            }
        }

        // Display retry message to user
        char retry_msg[512];
        const char *error_type = (result.http_status == 429) ? "Rate limit" :
                                (result.http_status == 408) ? "Request timeout" :
                                (result.http_status >= 500) ? "Server error" : "Error";
        snprintf(retry_msg, sizeof(retry_msg),
                "%s - retrying in %d ms... (attempt %d)",
                error_type, delay_ms, attempt_num + 1);
        print_error(retry_msg);

        LOG_INFO("Retrying after %d ms (elapsed: %ld ms, remaining: %ld ms)",
                delay_ms, elapsed_ms, remaining_ms);

        // Sleep and retry
        usleep((useconds_t)(delay_ms * 1000));
        backoff_ms = (int)(backoff_ms * BACKOFF_MULTIPLIER);
        if (backoff_ms > MAX_BACKOFF_MS) {
            backoff_ms = MAX_BACKOFF_MS;
        }

        free(result.raw_response);
        free(result.request_json);
        free(result.error_message);
        attempt_num++;
    }
}

/**
 * Main API call entry point
 */
static ApiResponse* call_api(ConversationState *state) {
    return call_api_with_retries(state);
}


// ============================================================================
// Context Building - Environment and Git Information
// ============================================================================

// Get current date in YYYY-MM-DD format
static char* get_current_date(void) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    char *date = malloc(11); // "YYYY-MM-DD\0"
    if (!date) return NULL;

    strftime(date, 11, "%Y-%m-%d", tm_info);
    return date;
}

// Check if current directory is a git repository
static int is_git_repo(const char *working_dir) {
    char git_path[PATH_MAX];
    snprintf(git_path, sizeof(git_path), "%s/.git", working_dir);

    struct stat st;
    return (stat(git_path, &st) == 0);
}

// Execute git command and return output
static char* exec_git_command(const char *command) {
    FILE *fp = popen(command, "r");
    if (!fp) return NULL;

    char *output = NULL;
    size_t output_size = 0;
    char buffer[1024];

    while (fgets(buffer, sizeof(buffer), fp)) {
        size_t len = strlen(buffer);
        char *new_output = realloc(output, output_size + len + 1);
        if (!new_output) {
            free(output);
            pclose(fp);
            return NULL;
        }
        output = new_output;
        memcpy(output + output_size, buffer, len);
        output_size += len;
        output[output_size] = '\0';
    }

    pclose(fp);

    // Trim trailing newline
    if (output && output_size > 0 && output[output_size-1] == '\n') {
        output[output_size-1] = '\0';
    }

    return output;
}

// Get git status information
static char* get_git_status(const char *working_dir) {
    if (!is_git_repo(working_dir)) {
        return NULL;
    }

    // Get current branch
    char *branch = exec_git_command("git rev-parse --abbrev-ref HEAD 2>/dev/null");
    if (!branch) branch = strdup("unknown");

    // Get git status (clean or modified)
    char *status_output = exec_git_command("git status --porcelain 2>/dev/null");
    const char *status = (status_output && strlen(status_output) > 0) ? "modified" : "clean";

    // Get recent commits (last 5)
    char *commits = exec_git_command("git log --oneline -5 2>/dev/null");
    if (!commits) commits = strdup("(no commits)");

    // Build the gitStatus block
    size_t total_size = 1024 + strlen(branch) + strlen(status) + strlen(commits);
    char *git_status = malloc(total_size);
    if (!git_status) {
        free(branch);
        free(status_output);
        free(commits);
        return NULL;
    }

    snprintf(git_status, total_size,
        "gitStatus: This is the git status at the start of the conversation. "
        "Note that this status is a snapshot in time, and will not update during the conversation.\n"
        "Current branch: %s\n\n"
        "Main branch (you will usually use this for PRs): \n\n"
        "Status:\n(%s)\n\n"
        "Recent commits:\n%s",
        branch, status, commits);

    free(branch);
    free(status_output);
    free(commits);

    return git_status;
}

// Get OS/Platform information
static char* get_os_version(void) {
    char *os_version = exec_git_command("uname -sr 2>/dev/null");
    if (!os_version) {
        os_version = strdup("Unknown");
    }
    return os_version;
}

static const char* get_platform(void) {
#ifdef __APPLE__
    return "darwin";
#elif defined(__linux__)
    return "linux";
#elif defined(_WIN32) || defined(_WIN64)
    return "win32";
#elif defined(__FreeBSD__)
    return "freebsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#else
    return "unknown";
#endif
}

// Read CLAUDE.md from working directory if it exists
static char* read_claude_md(const char *working_dir) {
    char claude_md_path[PATH_MAX];
    snprintf(claude_md_path, sizeof(claude_md_path), "%s/CLAUDE.md", working_dir);

    // Check if file exists
    struct stat st;
    if (stat(claude_md_path, &st) != 0) {
        return NULL; // File doesn't exist
    }

    // Read the file
    FILE *f = fopen(claude_md_path, "r");
    if (!f) {
        return NULL;
    }

    // Allocate buffer based on file size
    size_t file_size = (size_t)st.st_size;
    char *content = malloc(file_size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t read_size = fread(content, 1, file_size, f);
    fclose(f);

    if (read_size != file_size) {
        free(content);
        return NULL;
    }

    content[file_size] = '\0';
    return content;
}

// Build complete system prompt with environment context
char* build_system_prompt(ConversationState *state) {
    const char *working_dir = state->working_dir;
    char *date = get_current_date();
    const char *platform = get_platform();
    char *os_version = get_os_version();
    int is_git = is_git_repo(working_dir);
    char *git_status = is_git ? get_git_status(working_dir) : NULL;
    char *claude_md = read_claude_md(working_dir);

    // Calculate required buffer size
    size_t prompt_size = 2048; // Base size for the prompt template
    if (git_status) prompt_size += strlen(git_status);
    if (claude_md) prompt_size += strlen(claude_md) + 512; // Extra space for formatting

    // Add space for additional directories
    for (int i = 0; i < state->additional_dirs_count; i++) {
        prompt_size += strlen(state->additional_dirs[i]) + 4; // path + ", " separator
    }

    char *prompt = malloc(prompt_size);
    if (!prompt) {
        free(date);
        free(os_version);
        free(git_status);
        return NULL;
    }

    // Build the system prompt with additional directories
    int offset = snprintf(prompt, prompt_size,
        "Here is useful information about the environment you are running in:\n"
        "<env>\n"
        "Working directory: %s\n"
        "Additional working directories: ",
        working_dir);

    // Add additional directories
    if (state->additional_dirs_count > 0) {
        for (int i = 0; i < state->additional_dirs_count; i++) {
            if (i > 0) {
                offset += snprintf(prompt + offset, prompt_size - (size_t)offset, ", ");
            }
            offset += snprintf(prompt + offset, prompt_size - (size_t)offset, "%s", state->additional_dirs[i]);
        }
    }
    offset += snprintf(prompt + offset, prompt_size - (size_t)offset, "\n");

    offset += snprintf(prompt + offset, prompt_size - (size_t)offset,
        "Is directory a git repo: %s\n"
        "Platform: %s\n"
        "OS Version: %s\n"
        "Today's date: %s\n"
        "</env>\n",
        is_git ? "Yes" : "No",
        platform,
        os_version,
        date);

    // Add git status if available
    if (git_status && offset < (int)prompt_size) {
        offset += snprintf(prompt + offset, prompt_size - (size_t)offset, "\n%s\n", git_status);
    }

    // Add CLAUDE.md content if available
    if (claude_md && offset < (int)prompt_size) {
        offset += snprintf(prompt + offset, prompt_size - (size_t)offset,
            "\n<system-reminder>\n"
            "As you answer the user's questions, you can use the following context:\n"
            "# claudeMd\n"
            "Codebase and user instructions are shown below. Be sure to adhere to these instructions. "
            "IMPORTANT: These instructions OVERRIDE any default behavior and you MUST follow them exactly as written.\n\n"
            "Contents of %s/CLAUDE.md (project instructions, checked into the codebase):\n\n"
            "%s\n\n"
            "      IMPORTANT: this context may or may not be relevant to your tasks. "
            "You should not respond to this context unless it is highly relevant to your task.\n"
            "</system-reminder>\n",
            working_dir, claude_md);
    }

    free(date);
    free(os_version);
    free(git_status);
    free(claude_md);

    return prompt;
}

// ============================================================================ 
// Message Management
// ============================================================================

int conversation_state_init(ConversationState *state) {
    if (!state) {
        return -1;
    }

    if (state->conv_mutex_initialized) {
        return 0;
    }

    if (pthread_mutex_init(&state->conv_mutex, NULL) != 0) {
        LOG_ERROR("Failed to initialize conversation mutex");
        return -1;
    }

    state->conv_mutex_initialized = 1;
    return 0;
}

void conversation_state_destroy(ConversationState *state) {
    if (!state || !state->conv_mutex_initialized) {
        return;
    }

    pthread_mutex_destroy(&state->conv_mutex);
    state->conv_mutex_initialized = 0;
}

int conversation_state_lock(ConversationState *state) {
    if (!state) {
        return -1;
    }

    if (!state->conv_mutex_initialized) {
        if (conversation_state_init(state) != 0) {
            return -1;
        }
    }

    if (pthread_mutex_lock(&state->conv_mutex) != 0) {
        LOG_ERROR("Failed to lock conversation mutex");
        return -1;
    }
    return 0;
}

void conversation_state_unlock(ConversationState *state) {
    if (!state || !state->conv_mutex_initialized) {
        return;
    }
    pthread_mutex_unlock(&state->conv_mutex);
}

static void add_system_message(ConversationState *state, const char *text) {
    if (conversation_state_lock(state) != 0) {
        return;
    }

    if (state->count >= MAX_MESSAGES) {
        LOG_ERROR("Maximum message count reached");
        conversation_state_unlock(state);
        return;
    }

    InternalMessage *msg = &state->messages[state->count++];
    msg->role = MSG_SYSTEM;
    msg->contents = calloc(1, sizeof(InternalContent));
    if (!msg->contents) {
        LOG_ERROR("Failed to allocate memory for message content");
        state->count--;
        return;
    }
    msg->content_count = 1;
    // calloc already zeros memory, but explicitly set for analyzer
    msg->contents[0].type = INTERNAL_TEXT;
    msg->contents[0].text = NULL;
    msg->contents[0].tool_id = NULL;
    msg->contents[0].tool_name = NULL;
    msg->contents[0].tool_params = NULL;
    msg->contents[0].tool_output = NULL;

    msg->contents[0].text = strdup(text);
    if (!msg->contents[0].text) {
        LOG_ERROR("Failed to duplicate message text");
        free(msg->contents);
        msg->contents = NULL;
        state->count--;
        conversation_state_unlock(state);
        return;
    }

    conversation_state_unlock(state);
}

static void add_user_message(ConversationState *state, const char *text) {
    if (conversation_state_lock(state) != 0) {
        return;
    }

    if (state->count >= MAX_MESSAGES) {
        LOG_ERROR("Maximum message count reached");
        conversation_state_unlock(state);
        return;
    }

    InternalMessage *msg = &state->messages[state->count++];
    msg->role = MSG_USER;
    msg->contents = calloc(1, sizeof(InternalContent));
    if (!msg->contents) {
        LOG_ERROR("Failed to allocate memory for message content");
        state->count--; // Rollback count increment
        return;
    }
    msg->content_count = 1;
    // calloc already zeros memory, but explicitly set for analyzer
    msg->contents[0].type = INTERNAL_TEXT;
    msg->contents[0].text = NULL;
    msg->contents[0].tool_id = NULL;
    msg->contents[0].tool_name = NULL;
    msg->contents[0].tool_params = NULL;
    msg->contents[0].tool_output = NULL;

    msg->contents[0].text = strdup(text);
    if (!msg->contents[0].text) {
        LOG_ERROR("Failed to duplicate message text");
        free(msg->contents);
        msg->contents = NULL;
        state->count--; // Rollback count increment
        conversation_state_unlock(state);
        return;
    }

    conversation_state_unlock(state);
}

// Parse OpenAI message format and add to conversation
static void add_assistant_message_openai(ConversationState *state, cJSON *message) {
    if (conversation_state_lock(state) != 0) {
        return;
    }

    if (state->count >= MAX_MESSAGES) {
        LOG_ERROR("Maximum message count reached");
        conversation_state_unlock(state);
        return;
    }

    InternalMessage *msg = &state->messages[state->count++];
    msg->role = MSG_ASSISTANT;

    // Count content blocks (text + tool calls)
    int content_count = 0;
    cJSON *content = cJSON_GetObjectItem(message, "content");
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");

    if (content && cJSON_IsString(content) && content->valuestring) {
        content_count++;
    }

    // Count VALID tool calls (those with 'function' field)
    int tool_calls_count = 0;
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        int array_size = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < array_size; i++) {
            cJSON *tool_call = cJSON_GetArrayItem(tool_calls, i);
            cJSON *function = cJSON_GetObjectItem(tool_call, "function");
            cJSON *id = cJSON_GetObjectItem(tool_call, "id");
            if (function && id && cJSON_IsString(id)) {
                tool_calls_count++;
            } else {
                LOG_WARN("Skipping malformed tool_call at index %d (missing 'function' or 'id' field)", i);
            }
        }
        content_count += tool_calls_count;
    }

    // Ensure we have at least some content
    if (content_count == 0) {
        LOG_WARN("Assistant message has no content");
        state->count--; // Rollback count increment
        conversation_state_unlock(state);
        return;
    }

    msg->contents = calloc((size_t)content_count, sizeof(InternalContent));
    if (!msg->contents) {
        LOG_ERROR("Failed to allocate memory for message content");
        state->count--; // Rollback count increment
        conversation_state_unlock(state);
        return;
    }
    msg->content_count = content_count;

    int idx = 0;

    // Add text content if present
    if (content && cJSON_IsString(content) && content->valuestring) {
        msg->contents[idx].type = INTERNAL_TEXT;
        msg->contents[idx].text = strdup(content->valuestring);
        if (!msg->contents[idx].text) {
            LOG_ERROR("Failed to duplicate message text");
            free(msg->contents);
            msg->contents = NULL;
            state->count--;
            conversation_state_unlock(state);
            return;
        }
        idx++;
    }

    // Add tool calls if present
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        int array_size = cJSON_GetArraySize(tool_calls);
        for (int i = 0; i < array_size; i++) {
            cJSON *tool_call = cJSON_GetArrayItem(tool_calls, i);
            cJSON *id = cJSON_GetObjectItem(tool_call, "id");
            cJSON *function = cJSON_GetObjectItem(tool_call, "function");

            // Skip malformed tool calls (already logged warning during counting)
            if (!function || !id || !cJSON_IsString(id)) {
                continue;
            }

            cJSON *name = cJSON_GetObjectItem(function, "name");
            cJSON *arguments = cJSON_GetObjectItem(function, "arguments");

            msg->contents[idx].type = INTERNAL_TOOL_CALL;
            msg->contents[idx].tool_id = strdup(id->valuestring);
            if (!msg->contents[idx].tool_id) {
                LOG_ERROR("Failed to duplicate tool use ID");
                // Cleanup previously allocated content
                for (int j = 0; j < idx; j++) {
                    free(msg->contents[j].text);
                    free(msg->contents[j].tool_id);
                    free(msg->contents[j].tool_name);
                }
                free(msg->contents);
                msg->contents = NULL;
                state->count--;
                conversation_state_unlock(state);
                return;
            }
            msg->contents[idx].tool_name = strdup(name->valuestring);
            if (!msg->contents[idx].tool_name) {
                LOG_ERROR("Failed to duplicate tool name");
                free(msg->contents[idx].tool_id);
                // Cleanup previously allocated content
                for (int j = 0; j < idx; j++) {
                    free(msg->contents[j].text);
                    free(msg->contents[j].tool_id);
                    free(msg->contents[j].tool_name);
                }
                free(msg->contents);
                msg->contents = NULL;
                state->count--;
                conversation_state_unlock(state);
                return;
            }

            // Parse arguments string as JSON
            if (arguments && cJSON_IsString(arguments)) {
                msg->contents[idx].tool_params = cJSON_Parse(arguments->valuestring);
            } else {
                msg->contents[idx].tool_params = cJSON_CreateObject();
            }
            idx++;
        }
    }

    conversation_state_unlock(state);
}

// Helper: Free an array of InternalContent and its internal allocations
static void free_internal_contents(InternalContent *results, int count) {
    if (!results) return;
    for (int i = 0; i < count; i++) {
        InternalContent *cb = &results[i];
        free(cb->text);
        free(cb->tool_id);
        free(cb->tool_name);
        if (cb->tool_params) cJSON_Delete(cb->tool_params);
        if (cb->tool_output) cJSON_Delete(cb->tool_output);
    }
    free(results);
}

static void add_tool_results(ConversationState *state, InternalContent *results, int count) {
    if (conversation_state_lock(state) != 0) {
        free_internal_contents(results, count);
        return;
    }

    if (state->count >= MAX_MESSAGES) {
        LOG_ERROR("Maximum message count reached");
        // Free results since they won't be added to state
        free_internal_contents(results, count);
        conversation_state_unlock(state);
        return;
    }

    InternalMessage *msg = &state->messages[state->count++];
    msg->role = MSG_USER;
    msg->contents = results;
    msg->content_count = count;

    conversation_state_unlock(state);
}

// ============================================================================
// Interactive Mode - Simple Terminal I/O
// ============================================================================

void clear_conversation(ConversationState *state) {
    if (conversation_state_lock(state) != 0) {
        return;
    }

    // Keep the system message (first message)
    int system_msg_count = 0;

    if (state->count > 0 && state->messages[0].role == MSG_SYSTEM) {
        // System message remains intact
        system_msg_count = 1;
    }

    // Free all other message content
    for (int i = system_msg_count; i < state->count; i++) {
        for (int j = 0; j < state->messages[i].content_count; j++) {
            InternalContent *cb = &state->messages[i].contents[j];
            free(cb->text);
            free(cb->tool_id);
            free(cb->tool_name);
            if (cb->tool_params) cJSON_Delete(cb->tool_params);
            if (cb->tool_output) cJSON_Delete(cb->tool_output);
        }
        free(state->messages[i].contents);
    }

    // Reset message count (keeping system message)
    state->count = system_msg_count;

    // Clear todo list
    if (state->todo_list) {
        todo_free(state->todo_list);
        todo_init(state->todo_list);
        LOG_DEBUG("Todo list cleared and reinitialized");
    }

    conversation_state_unlock(state);
}

// Free all messages and their contents (including system message). Use at program shutdown.
void conversation_free(ConversationState *state) {
    if (conversation_state_lock(state) != 0) {
        return;
    }

    // Free all messages
    for (int i = 0; i < state->count; i++) {
        for (int j = 0; j < state->messages[i].content_count; j++) {
            InternalContent *cb = &state->messages[i].contents[j];
            free(cb->text);
            free(cb->tool_id);
            free(cb->tool_name);
            if (cb->tool_params) cJSON_Delete(cb->tool_params);
            if (cb->tool_output) cJSON_Delete(cb->tool_output);
        }
        free(state->messages[i].contents);
    }
    state->count = 0;

    // Note: todo_list is freed separately in main cleanup
    // Do not call todo_free() here to avoid double-free

    conversation_state_unlock(state);
}

static void process_response(ConversationState *state,
                             ApiResponse *response,
                             TUIState *tui,
                             TUIMessageQueue *queue) {
    // Time the entire response processing
    struct timespec proc_start, proc_end;
    clock_gettime(CLOCK_MONOTONIC, &proc_start);

    // Display assistant's text content if present
    if (response->message.text && response->message.text[0] != '\0') {
        // Skip whitespace-only content
        const char *p = response->message.text;
        while (*p && isspace((unsigned char)*p)) p++;

        if (*p != '\0') {  // Has non-whitespace content
            ui_append_line(tui, queue, "[Assistant]", p, COLOR_PAIR_ASSISTANT);
        }
    }

    // Add to conversation history (using raw response for now)
    // Extract message from raw_response for backward compatibility
    cJSON *choices = cJSON_GetObjectItem(response->raw_response, "choices");
    if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(choice, "message");
        if (message) {
            add_assistant_message_openai(state, message);
        }
    }

    // Process tool calls from vendor-agnostic structure
    int tool_count = response->tool_count;
    ToolCall *tool_calls_array = response->tools;

    if (tool_count > 0) {

        LOG_INFO("Processing %d tool call(s)", tool_count);

        // Time tool execution phase
        struct timespec tool_start, tool_end;
        clock_gettime(CLOCK_MONOTONIC, &tool_start);

        // Parallel tool execution
        InternalContent *results = calloc((size_t)tool_count, sizeof(InternalContent));
        // results will be owned by ConversationState after add_tool_results().
        // Use free_internal_contents() to free in early exits.
        pthread_t *threads = malloc((size_t)tool_count * sizeof(pthread_t));
        ToolThreadArg *args = malloc((size_t)tool_count * sizeof(ToolThreadArg));
        int thread_count = 0;

        // Launch tool execution threads
        for (int i = 0; i < tool_count; i++) {
            ToolCall *tool = &tool_calls_array[i];

            if (!tool->name || !tool->id) {
                // This should never happen - provider should sanitize responses
                LOG_ERROR("Tool call missing name or id (provider validation failed)");
                continue;
            }

            // Use parameters directly (already parsed by provider)
            // Duplicate parameters for thread to own and delete
            cJSON *input;
            if (tool->parameters) {
                input = cJSON_Duplicate(tool->parameters, /*recurse*/1);
            } else {
                input = cJSON_CreateObject();
            }

            char *tool_details = get_tool_details(tool->name, input);

            char prefix_with_tool[128];
            snprintf(prefix_with_tool, sizeof(prefix_with_tool), "[Tool: %s]", tool->name);
            ui_append_line(tui, queue, prefix_with_tool, tool_details, COLOR_PAIR_TOOL);

            // Prepare thread arguments
            args[thread_count].tool_use_id = strdup(tool->id);
            args[thread_count].tool_name = tool->name;
            args[thread_count].input = input;
            args[thread_count].state = state;
            args[thread_count].result_block = &results[i];

            // Create thread
            pthread_create(&threads[thread_count], NULL, tool_thread_func, &args[thread_count]);
            thread_count++;
        }

        // Show spinner or status while waiting for tools to complete
        Spinner *tool_spinner = NULL;
        if (!tui && !queue) {
            char spinner_msg[128];
            snprintf(spinner_msg, sizeof(spinner_msg), "Running %d tool%s...",
                     thread_count, thread_count > 1 ? "s" : "");
            tool_spinner = spinner_start(spinner_msg, SPINNER_YELLOW);
        } else {
            char status_msg[128];
            snprintf(status_msg, sizeof(status_msg), "Running %d tool%s...",
                     thread_count, thread_count > 1 ? "s" : "");
            ui_set_status(tui, queue, status_msg);
        }

        // Wait for all tool threads to complete, checking for ESC periodically
        // Use a helper thread to monitor tool completion
        int interrupted = 0;
        volatile int all_tools_done = 0;

        // Create a monitor thread that joins all tool threads
        pthread_t monitor_thread;
        MonitorArg monitor_arg = {threads, thread_count, &all_tools_done};
        pthread_create(&monitor_thread, NULL, tool_monitor_func, &monitor_arg);

        // Now check for ESC while waiting for tools to complete
        while (!all_tools_done) {
            if (check_for_esc()) {
                LOG_INFO("Tool execution interrupted by user (ESC pressed) - cancelling threads");
                interrupted = 1;

                // Cancel all tool threads immediately
                for (int i = 0; i < thread_count; i++) {
                    pthread_cancel(threads[i]);
                }

                // Update status to show immediate termination
                if (!tui && !queue) {
                    if (tool_spinner) {
                        spinner_update(tool_spinner, "Interrupted (ESC) - terminating tools...");
                    }
                } else {
                    ui_set_status(tui, queue, "Interrupted (ESC) - terminating tools...");
                }
                break;
            }
            usleep(50000);  // Check every 50ms
        }

        // Wait for monitor thread to complete (this ensures all tool threads are joined)
        // If interrupted, threads are cancelled; otherwise wait for normal completion
        pthread_join(monitor_thread, NULL);

        clock_gettime(CLOCK_MONOTONIC, &tool_end);
        long tool_exec_ms = (tool_end.tv_sec - tool_start.tv_sec) * 1000 +
                            (tool_end.tv_nsec - tool_start.tv_nsec) / 1000000;
        LOG_INFO("All %d tool(s) completed in %ld ms", thread_count, tool_exec_ms);

        // If interrupted, clean up and return
        if (interrupted) {
            if (!tui && !queue) {
                if (tool_spinner) {
                    spinner_stop(tool_spinner, "Interrupted by user (ESC) - tools terminated", 0);
                }
            } else {
                ui_set_status(tui, queue, "Interrupted by user (ESC) - tools terminated");
            }

            free(threads);
            free(args);
            // results will be freed when conversation state is cleaned up
            return;  // Exit without continuing conversation
        }

        // Check if any tools had errors and display error messages
        int has_error = 0;
        for (int i = 0; i < tool_count; i++) {
            if (results[i].is_error) {
                has_error = 1;

                // Extract and display the error message
                cJSON *error_obj = cJSON_GetObjectItem(results[i].tool_output, "error");
                const char *error_msg = error_obj && cJSON_IsString(error_obj)
                    ? error_obj->valuestring
                    : "Unknown error";

                // Get tool name for better context
                const char *tool_name = results[i].tool_name ? results[i].tool_name : "tool";

                // Display error next to/below the tool execution
                char error_display[512];
                snprintf(error_display, sizeof(error_display), "%s failed: %s", tool_name, error_msg);
                ui_show_error(tui, queue, error_display);
            }
        }

        // Clear status on success, show message on error
        if (!tui && !queue) {
            printf("DEBUG: has_error = %d\n", has_error);
            if (has_error) {
                spinner_stop(tool_spinner, "Tool execution completed with errors", 0);
            } else {
                printf("DEBUG: Stopping tool spinner with success message\n");
                spinner_stop(tool_spinner, "Tool execution completed successfully", 1);
            }
        } else {
            if (has_error) {
                ui_set_status(tui, queue, "Tool execution completed with errors");
            } else {
                ui_set_status(tui, queue, "");  // Clear status on success
            }
        }
        free(threads);
        free(args);

        // Add tool results and continue conversation
        add_tool_results(state, results, tool_count);

        Spinner *followup_spinner = NULL;
        if (!tui && !queue) {
            followup_spinner = spinner_start("Processing tool results...", SPINNER_CYAN);
        } else {
            ui_set_status(tui, queue, "Processing tool results...");
        }
        ApiResponse *next_response = call_api(state);
        // Clear status after API call completes
        if (!tui && !queue) {
            spinner_stop(followup_spinner, NULL, 1);  // Clear without message
        } else {
            ui_set_status(tui, queue, "");  // Clear status
        }
        if (next_response) {
            process_response(state, next_response, tui, queue);
            api_response_free(next_response);
        } else {
            // API call failed - show error to user
            const char *error_msg = "API call failed after executing tools. Check logs for details.";
            ui_show_error(tui, queue, error_msg);
            LOG_ERROR("API call returned NULL after tool execution");
        }

        // results is now owned by the conversation state and will be freed upon state cleanup

        clock_gettime(CLOCK_MONOTONIC, &proc_end);
        long proc_ms = (proc_end.tv_sec - proc_start.tv_sec) * 1000 +
                       (proc_end.tv_nsec - proc_start.tv_nsec) / 1000000;
        LOG_INFO("Response processing completed in %ld ms (tools: %ld ms, recursion included)",
                 proc_ms, tool_exec_ms);
        return;
    }

    // No tools - just log completion time
    clock_gettime(CLOCK_MONOTONIC, &proc_end);
    long proc_ms = (proc_end.tv_sec - proc_start.tv_sec) * 1000 +
                   (proc_end.tv_nsec - proc_start.tv_nsec) / 1000000;
    LOG_INFO("Response processing completed in %ld ms (no tools)", proc_ms);
}

static void ai_worker_handle_instruction(AIWorkerContext *ctx, const AIInstruction *instruction) {
    if (!ctx || !instruction) {
        return;
    }

    ui_set_status(NULL, ctx->tui_queue, "Waiting for API response...");

    ApiResponse *response = call_api(ctx->state);

    ui_set_status(NULL, ctx->tui_queue, "");

    if (!response) {
        ui_show_error(NULL, ctx->tui_queue, "Failed to get response from API");
        return;
    }

    cJSON *error = cJSON_GetObjectItem(response->raw_response, "error");
    if (error) {
        cJSON *error_message = cJSON_GetObjectItem(error, "message");
        const char *error_msg = error_message ? error_message->valuestring : "Unknown error";
        ui_show_error(NULL, ctx->tui_queue, error_msg);
        api_response_free(response);
        return;
    }

    process_response(ctx->state, response, NULL, ctx->tui_queue);
    api_response_free(response);
}

// ============================================================================
// Advanced Input Handler (readline-like)
// ============================================================================













typedef struct {
    ConversationState *state;
    TUIState *tui;
    AIWorkerContext *worker;
    AIInstructionQueue *instruction_queue;
    TUIMessageQueue *tui_queue;
} InteractiveContext;

// Submit callback invoked by the TUI event loop when the user presses Enter
static int submit_input_callback(const char *input, void *user_data) {
    InteractiveContext *ctx = (InteractiveContext *)user_data;
    if (!ctx || !ctx->state || !ctx->tui || !input) {
        return 0;
    }

    if (input[0] == '\0') {
        return 0;
    }

    TUIState *tui = ctx->tui;
    ConversationState *state = ctx->state;
    AIWorkerContext *worker = ctx->worker;
    TUIMessageQueue *queue = ctx->tui_queue;

    char *input_copy = strdup(input);
    if (!input_copy) {
        ui_show_error(tui, queue, "Memory allocation failed");
        return 0;
    }

    if (input_copy[0] == '/') {
        ui_append_line(tui, queue, "[User]", input_copy, COLOR_PAIR_USER);

        if (strcmp(input_copy, "/exit") == 0 || strcmp(input_copy, "/quit") == 0) {
            free(input_copy);
            return 1;
        } else if (strcmp(input_copy, "/clear") == 0) {
            clear_conversation(state);
            tui_clear_conversation(tui);
            ui_append_line(tui, queue, "[System]", "Conversation cleared", COLOR_PAIR_STATUS);
            free(input_copy);
            return 0;
        } else if (strncmp(input_copy, "/add-dir ", 9) == 0) {
            const char *path = input_copy + 9;
            if (add_directory(state, path) == 0) {
                ui_append_line(tui, queue, "[System]", "Directory added successfully", COLOR_PAIR_STATUS);
                char *new_system_prompt = build_system_prompt(state);
                if (!new_system_prompt) {
                    ui_show_error(tui, queue, "Failed to rebuild system prompt");
                    free(input_copy);
                    return 0;
                }

                if (state->count > 0 && state->messages[0].role == MSG_SYSTEM) {
                    free(state->messages[0].contents[0].text);
                    state->messages[0].contents[0].text = strdup(new_system_prompt);
                    if (!state->messages[0].contents[0].text) {
                        ui_show_error(tui, queue, "Memory allocation failed");
                    }
                }
                free(new_system_prompt);
            } else {
                ui_show_error(tui, queue, "Failed to add directory");
            }
            free(input_copy);
            return 0;
        } else if (strcmp(input_copy, "/help") == 0) {
            ui_append_line(tui, queue, "[System]", "Available commands:", COLOR_PAIR_STATUS);
            ui_append_line(tui, queue, "[System]", "  /exit, /quit - Exit the program", COLOR_PAIR_STATUS);
            ui_append_line(tui, queue, "[System]", "  /clear - Clear conversation history", COLOR_PAIR_STATUS);
            ui_append_line(tui, queue, "[System]", "  /add-dir <path> - Add additional working directory", COLOR_PAIR_STATUS);
            ui_append_line(tui, queue, "[System]", "  /help - Show this help message", COLOR_PAIR_STATUS);
            free(input_copy);
            return 0;
        } else {
            ui_show_error(tui, queue, "Unknown command. Type /help for available commands.");
            free(input_copy);
            return 0;
        }
    }

    ui_append_line(tui, queue, "[User]", input_copy, COLOR_PAIR_USER);
    add_user_message(state, input_copy);

    if (worker) {
        if (ai_worker_submit(worker, input_copy) != 0) {
            ui_show_error(tui, queue, "Failed to queue instruction for processing");
        } else {
            ui_set_status(tui, queue, "Instruction queued for processing...");
        }
    } else {
        ui_set_status(tui, queue, "Waiting for API response...");
        ApiResponse *response = call_api(state);
        ui_set_status(tui, queue, "");

        if (!response) {
            ui_show_error(tui, queue, "Failed to get response from API");
            free(input_copy);
            return 0;
        }

        cJSON *error = cJSON_GetObjectItem(response->raw_response, "error");
        if (error) {
            cJSON *error_message = cJSON_GetObjectItem(error, "message");
            const char *error_msg = error_message ? error_message->valuestring : "Unknown error";
            ui_show_error(tui, queue, error_msg);
            api_response_free(response);
            free(input_copy);
            return 0;
        }

        process_response(state, response, tui, queue);
        api_response_free(response);
    }

    free(input_copy);
    return 0;
}

// Advanced input handler with readline-like keybindings, driven by non-blocking event loop
static void interactive_mode(ConversationState *state) {
    // Display startup banner with theme colors (colorscheme already initialized in main())
    char color_code[32];
    const char *banner_color;
    if (get_colorscheme_color(COLORSCHEME_ASSISTANT, color_code, sizeof(color_code)) == 0) {
        banner_color = color_code;
    } else {
        LOG_WARN("Using fallback ANSI color for ASSISTANT (banner)");
        banner_color = ANSI_FALLBACK_BOLD_BLUE;  // Bold blue fallback from centralized system
    }

    printf("%s", banner_color);
    printf(" ▐▛███▜▌   claude-c v%s\n", VERSION);
    printf("▝▜█████▛▘  %s\n", state->model);
    printf("  ▘▘ ▝▝    %s\n", state->working_dir);
    printf(ANSI_RESET "\n");  // Reset color and add blank line
    fflush(stdout);

    // Initialize TUI
    TUIState tui = {0};
    if (tui_init(&tui) != 0) {
        LOG_ERROR("Failed to initialize TUI");
        return;
    }

    // Initialize command system
    commands_init();

    // Build initial status line
    char status_msg[256];
    snprintf(status_msg, sizeof(status_msg), "Model: %s | Session: %s | Commands: /exit /quit /clear /add-dir /help | Ctrl+D to exit",
             state->model, state->session_id ? state->session_id : "none");
    tui_update_status(&tui, status_msg);

    const size_t TUI_QUEUE_CAPACITY = 256;
    const size_t AI_QUEUE_CAPACITY = 16;
    TUIMessageQueue tui_queue;
    AIInstructionQueue instruction_queue;
    AIWorkerContext worker_ctx = {0};
    int tui_queue_initialized = 0;
    int instruction_queue_initialized = 0;
    int worker_started = 0;
    int async_enabled = 1;

    if (tui_msg_queue_init(&tui_queue, TUI_QUEUE_CAPACITY) != 0) {
        ui_show_error(&tui, NULL, "Failed to initialize TUI message queue; running in synchronous mode.");
        async_enabled = 0;
    } else {
        tui_queue_initialized = 1;
    }

    if (async_enabled) {
        if (ai_queue_init(&instruction_queue, AI_QUEUE_CAPACITY) != 0) {
            ui_show_error(&tui, NULL, "Failed to initialize instruction queue; running in synchronous mode.");
            async_enabled = 0;
        } else {
            instruction_queue_initialized = 1;
        }
    }

    if (async_enabled) {
        if (ai_worker_start(&worker_ctx, state, &instruction_queue, &tui_queue, ai_worker_handle_instruction) != 0) {
            ui_show_error(&tui, NULL, "Failed to start AI worker thread; running in synchronous mode.");
            async_enabled = 0;
        } else {
            worker_started = 1;
        }
    }

    if (!async_enabled) {
        if (worker_started) {
            ai_worker_stop(&worker_ctx);
            worker_started = 0;
        }
        if (instruction_queue_initialized) {
            ai_queue_free(&instruction_queue);
            instruction_queue_initialized = 0;
        }
        if (tui_queue_initialized) {
            tui_msg_queue_shutdown(&tui_queue);
            tui_msg_queue_free(&tui_queue);
            tui_queue_initialized = 0;
        }
    }

    InteractiveContext ctx = {
        .state = state,
        .tui = &tui,
        .worker = worker_started ? &worker_ctx : NULL,
        .instruction_queue = instruction_queue_initialized ? &instruction_queue : NULL,
        .tui_queue = tui_queue_initialized ? &tui_queue : NULL,
    };

    void *event_loop_queue = tui_queue_initialized ? (void *)&tui_queue : NULL;
    tui_event_loop(&tui, ">", submit_input_callback, &ctx, event_loop_queue);

    if (worker_started) {
        ai_worker_stop(&worker_ctx);
    }
    if (instruction_queue_initialized) {
        ai_queue_free(&instruction_queue);
    }
    if (tui_queue_initialized) {
        tui_msg_queue_shutdown(&tui_queue);
        tui_msg_queue_free(&tui_queue);
    }

    // Cleanup TUI
    tui_cleanup(&tui);
    printf("Goodbye!\n");
}

// ============================================================================
// Session ID Generation
// ============================================================================

// Generate a unique session ID using timestamp and random data
// Helper function to get integer value from environment variable with default
static int get_env_int_retry(const char *name, int default_value) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0') {
        return default_value;
    }

    char *endptr;
    long result = strtol(value, &endptr, 10);
    if (*endptr != '\0' || result < 0 || result > INT_MAX) {
        LOG_WARN("Invalid value for %s: '%s', using default %d", name, value, default_value);
        return default_value;
    }

    return (int)result;
}

// Format: sess_<timestamp>_<random>
// Returns: Newly allocated string (caller must free)
static char* generate_session_id(void) {
    char *session_id = malloc(64);
    if (!session_id) {
        return NULL;
    }

    // Seed random with current time + process ID for uniqueness
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    srand((unsigned int)(ts.tv_sec ^ ts.tv_nsec ^ getpid()));

    // Generate session ID: sess_<unix_timestamp>_<random_hex>
    unsigned int random_part = (unsigned int)rand();
    snprintf(session_id, 64, "sess_%ld_%08x", ts.tv_sec, random_part);

    return session_id;
}

// ============================================================================
// Main Entry Point
#ifndef TEST_BUILD
// ============================================================================

int main(int argc, char *argv[]) {
    // Handle version flag first (no API key needed)
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("Claude C version %s\n", CLAUDE_C_VERSION_FULL);
        return 0;
    }

    // Handle help flag first (no API key needed)
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("Claude Code - Pure C Implementation (OpenAI Compatible)\n");
        printf("Version: %s\n\n", CLAUDE_C_VERSION_FULL);
        printf("Usage:\n");
        printf("  %s               Start interactive mode\n", argv[0]);
        printf("  %s -h, --help    Show this help message\n", argv[0]);
        printf("  %s --version     Show version information\n\n", argv[0]);
        printf("Environment Variables:\n");
        printf("  API Configuration:\n");
        printf("    OPENAI_API_KEY       Required: Your OpenAI API key (not needed for Bedrock)\n");
        printf("    OPENAI_API_BASE      Optional: API base URL (default: %s)\n", API_BASE_URL);
        printf("    OPENAI_MODEL         Optional: Model name (default: %s)\n", DEFAULT_MODEL);
        printf("    ANTHROPIC_MODEL      Alternative: Model name (fallback if OPENAI_MODEL not set)\n");
        printf("    DISABLE_PROMPT_CACHING  Optional: Set to 1 to disable prompt caching\n\n");
        printf("  AWS Bedrock Configuration:\n");
        printf("    CLAUDE_CODE_USE_BEDROCK  Set to 1 to use AWS Bedrock instead of OpenAI\n");
        printf("    ANTHROPIC_MODEL         Required for Bedrock: Claude model ID\n");
        printf("                            Examples: anthropic.claude-3-sonnet-20240229-v1:0\n");
        printf("                                      us.anthropic.claude-sonnet-4-5-20250929-v1:0\n");
        printf("    AWS credentials        Required: Configure via AWS CLI or environment\n\n");
        printf("  Logging and Persistence:\n");
        printf("    CLAUDE_C_LOG_PATH    Optional: Full path to log file\n");
        printf("    CLAUDE_C_LOG_DIR     Optional: Directory for logs (uses claude.log filename)\n");
        printf("    CLAUDE_LOG_LEVEL     Optional: Log level (DEBUG, INFO, WARN, ERROR)\n");
        printf("    CLAUDE_C_DB_PATH     Optional: Path to SQLite database for API history\n");
        printf("                         Default: ~/.local/share/claude-c/api_calls.db\n");
        printf("    CLAUDE_C_MAX_RETRY_DURATION_MS  Optional: Maximum retry duration in milliseconds\n");
        printf("                                     Default: 600000 (10 minutes)\n\n");
        printf("  UI Customization:\n");
        printf("    CLAUDE_C_THEME       Optional: Path to Kitty theme file\n\n");
        return 0;
    }

    // Check that no extra arguments were provided
    if (argc > 1) {
        LOG_ERROR("Unexpected arguments provided");
        printf("Try '%s --help' for usage information.\n", argv[0]);
        return 1;
    }

#ifndef TEST_BUILD
    // Check if Bedrock mode is enabled
    int use_bedrock = bedrock_is_enabled();
#else
    int use_bedrock = 0;
#endif

    const char *api_key = NULL;
    const char *api_base = NULL;
    const char *model = NULL;

    if (use_bedrock) {
        // Bedrock mode: API key not required, credentials loaded separately
        // Get model from ANTHROPIC_MODEL environment variable
        model = getenv("ANTHROPIC_MODEL");
        if (!model) {
            LOG_ERROR("ANTHROPIC_MODEL environment variable required when using AWS Bedrock");
            fprintf(stderr, "Error: ANTHROPIC_MODEL environment variable not set\n");
            fprintf(stderr, "Example: export ANTHROPIC_MODEL=us.anthropic.claude-sonnet-4-5-20250929-v1:0\n");
            return 1;
        }
        // API key and base URL will be handled by Bedrock module
        api_key = "bedrock";  // Placeholder
        api_base = "bedrock"; // Will be overridden by Bedrock endpoint
        LOG_INFO("Bedrock mode enabled, using model: %s", model);
    } else {
        // Standard mode: check for API key
        api_key = getenv("OPENAI_API_KEY");
        if (!api_key) {
            LOG_ERROR("OPENAI_API_KEY environment variable not set");
            fprintf(stderr, "Error: OPENAI_API_KEY environment variable not set\n");
            fprintf(stderr, "\nTo use AWS Bedrock instead, set:\n");
            fprintf(stderr, "  export CLAUDE_CODE_USE_BEDROCK=true\n");
            fprintf(stderr, "  export ANTHROPIC_MODEL=us.anthropic.claude-sonnet-4-5-20250929-v1:0\n");
            fprintf(stderr, "  export AWS_REGION=us-west-2\n");
            fprintf(stderr, "  export AWS_PROFILE=your-profile\n");
            return 1;
        }

        // Get optional API base and model from environment
        api_base = getenv("OPENAI_API_BASE");
        if (!api_base) {
            api_base = API_BASE_URL;
        }

        model = getenv("OPENAI_MODEL");
        if (!model) {
            model = getenv("ANTHROPIC_MODEL");  // Try ANTHROPIC_MODEL as fallback
            if (!model) {
                model = DEFAULT_MODEL;
            }
        }
    }

    // Initialize CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Initialize random number generator for retry jitter
    srand((unsigned int)time(NULL));

    // Initialize logging system
    if (log_init() != 0) {
        LOG_ERROR("Warning: Failed to initialize logging system");
    }

    // Configure log rotation: 10MB max size, keep 5 backups
    log_set_rotation(10, 5);

    // Set log level from environment or default to INFO
    const char *log_level_env = getenv("CLAUDE_LOG_LEVEL");
    if (log_level_env) {
        if (strcmp(log_level_env, "DEBUG") == 0) {
            log_set_level(LOG_LEVEL_DEBUG);
        } else if (strcmp(log_level_env, "WARN") == 0) {
            log_set_level(LOG_LEVEL_WARN);
        } else if (strcmp(log_level_env, "ERROR") == 0) {
            log_set_level(LOG_LEVEL_ERROR);
        }
    }

    LOG_INFO("Application started");
    LOG_INFO("API URL: %s", api_base);
    LOG_INFO("Model: %s", model);

    // Initialize colorscheme EARLY (before any colored output/spinners)
    const char *theme = getenv("CLAUDE_C_THEME");
    if (theme && strlen(theme) > 0) {
        char theme_path[512];
        // Check if theme is an absolute path or relative path
        if (theme[0] == '/' || theme[0] == '~' || strstr(theme, ".conf")) {
            // Absolute path or already has .conf extension - use as-is
            snprintf(theme_path, sizeof(theme_path), "%s", theme);
        } else {
            // Relative name without extension - add colorschemes/ prefix and .conf extension
            snprintf(theme_path, sizeof(theme_path), "colorschemes/%s.conf", theme);
        }
        if (init_colorscheme(theme_path) != 0) {
            LOG_WARN("Failed to load colorscheme '%s', will use ANSI fallback colors", theme);
        } else {
            LOG_DEBUG("Colorscheme loaded successfully from: %s", theme_path);
        }
    } else {
        // Try to load default theme
        if (init_colorscheme("colorschemes/kitty-default.conf") != 0) {
            LOG_DEBUG("No default colorscheme found, using ANSI fallback colors");
        } else {
            LOG_DEBUG("Default colorscheme loaded from: colorschemes/kitty-default.conf");
        }
    }

    // Initialize persistence layer
    PersistenceDB *persistence_db = persistence_init(NULL);  // NULL = use default path
    if (persistence_db) {
        LOG_INFO("Persistence layer initialized");
    } else {
        LOG_WARN("Failed to initialize persistence layer - API calls will not be logged");
    }

    // Generate unique session ID for this conversation
    char *session_id = generate_session_id();
    if (!session_id) {
        LOG_WARN("Failed to generate session ID");
    }
    LOG_INFO("Session ID: %s", session_id ? session_id : "none");

    // Set session ID for logging
    if (session_id) {
        log_set_session_id(session_id);
    }

    // Initialize conversation state
    ConversationState state = {0};
    if (conversation_state_init(&state) != 0) {
        LOG_ERROR("Failed to initialize conversation state synchronization");
        fprintf(stderr, "Error: Unable to initialize conversation state\n");
        free(session_id);
        if (persistence_db) {
            persistence_close(persistence_db);
        }
        curl_global_cleanup();
        log_shutdown();
        return 1;
    }
    state.api_key = strdup(api_key);
    state.api_url = strdup(api_base);
    state.model = strdup(model);
    state.working_dir = getcwd(NULL, 0);
    state.session_id = session_id;
    state.persistence_db = persistence_db;
    state.max_retry_duration_ms = get_env_int_retry("CLAUDE_C_MAX_RETRY_DURATION_MS", MAX_RETRY_DURATION_MS);

    // Initialize todo list
    state.todo_list = malloc(sizeof(TodoList));
    if (state.todo_list) {
        todo_init(state.todo_list);
        LOG_DEBUG("Todo list initialized");
    } else {
        LOG_ERROR("Failed to allocate memory for todo list");
    }

#ifndef TEST_BUILD
    // Initialize provider (OpenAI or Bedrock based on environment)
    ProviderInitResult provider_result;
    provider_init(model, state.api_key, &provider_result);
    if (!provider_result.provider) {
        LOG_ERROR("Failed to initialize provider: %s",
                  provider_result.error_message ? provider_result.error_message : "unknown error");
        fprintf(stderr, "Error: Failed to initialize API provider: %s\n",
                provider_result.error_message ? provider_result.error_message : "unknown error");
        free(provider_result.error_message);
        free(provider_result.api_url);
        free(state.api_key);
        free(state.api_url);
        free(state.model);
        free(state.working_dir);
        if (state.todo_list) {
            free(state.todo_list);
        }
        conversation_state_destroy(&state);
        curl_global_cleanup();
        return 1;
    }

    // Update api_url with provider's URL
    free(state.api_url);
    state.api_url = provider_result.api_url;  // Transfer ownership
    state.provider = provider_result.provider;  // Transfer ownership
    free(provider_result.error_message);  // Should be NULL on success, but free anyway

    LOG_INFO("Provider initialized: %s, API URL: %s", state.provider->name, state.api_url);
#else
    state.provider = NULL;
#endif

    // Check for allocation failures
    if (!state.api_key || !state.api_url || !state.model || !state.todo_list) {
        LOG_ERROR("Failed to allocate memory for conversation state");
        fprintf(stderr, "Error: Memory allocation failed\n");
        free(state.api_key);
        free(state.api_url);
        free(state.model);
        free(state.working_dir);
        if (state.todo_list) {
            free(state.todo_list);
        }
        conversation_state_destroy(&state);
        curl_global_cleanup();
        return 1;
    }

    if (!state.working_dir) {
        LOG_ERROR("Failed to get current working directory");
        free(state.api_key);
        free(state.api_url);
        free(state.model);
        conversation_state_destroy(&state);
        curl_global_cleanup();
        return 1;
    }

    LOG_INFO("API URL initialized: %s", state.api_url);

    // Build and add system prompt with environment context
    char *system_prompt = build_system_prompt(&state);
    if (system_prompt) {
        add_system_message(&state, system_prompt);

        // Debug: print system prompt if DEBUG_PROMPT environment variable is set
        if (getenv("DEBUG_PROMPT")) {
            printf("\n=== SYSTEM PROMPT (DEBUG) ===\n%s\n=== END SYSTEM PROMPT ===\n\n", system_prompt);
        }

        free(system_prompt);
        LOG_DEBUG("System prompt added with environment context");
    } else {
        LOG_WARN("Failed to build system prompt");
    }

    // Run interactive mode
    interactive_mode(&state);

    // Cleanup conversation messages
    conversation_free(&state);

    // Free additional directories
    for (int i = 0; i < state.additional_dirs_count; i++) {
        free(state.additional_dirs[i]);
    }
    free(state.additional_dirs);

    // Cleanup todo list
    if (state.todo_list) {
        todo_free(state.todo_list);
        free(state.todo_list);
        state.todo_list = NULL;
        LOG_DEBUG("Todo list cleaned up");
    }

#ifndef TEST_BUILD
    // Cleanup provider
    if (state.provider) {
        state.provider->cleanup(state.provider);
        LOG_DEBUG("Provider cleaned up");
    }
#endif

    free(state.api_key);
    free(state.api_url);
    free(state.model);
    free(state.working_dir);
    free(state.session_id);
    conversation_state_destroy(&state);

    // Close persistence layer
    if (state.persistence_db) {
        persistence_close(state.persistence_db);
        LOG_INFO("Persistence layer closed");
    }

    curl_global_cleanup();

    LOG_INFO("Application terminated");
    log_shutdown();

    return 0;
}

#endif // TEST_BUILD

#ifdef TEST_BUILD
#pragma GCC diagnostic pop
#endif
