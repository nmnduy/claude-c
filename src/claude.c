/*
 * Claude Code - Pure C Implementation
 * A lightweight coding agent that interacts with OpenAI-compatible APIs
 *
 * Compilation: make
 * Usage: ./claude "your prompt here"
 *
 * Dependencies: libcurl, cJSON, pthread
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
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

#ifdef TEST_BUILD
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
#else
// Normal build: use actual implementations
#include "logger.h"
#include "persistence.h"
#endif

// Visual indicators for interactive mode
#include "indicators.h"

// Internal API for module access
#include "claude_internal.h"
#include "todo.h"

// New input system modules
#include "lineedit.h"
#include "commands.h"

// TUI module
#include "tui.h"

#ifdef TEST_BUILD
#define main claude_main
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
        role_color_start = ANSI_FALLBACK_ASSISTANT;
    }
    
    // Get foreground color for main text
    if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
        text_color_start = text_color_code;
    } else {
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
        tool_color_start = ANSI_FALLBACK_TOOL;
    }
    
    // Get foreground color for details
    if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
        text_color_start = text_color_code;
    } else {
        text_color_start = ANSI_FALLBACK_FOREGROUND;
    }
    
    printf("%s[Tool: %s]%s", tool_color_start, tool_name, ANSI_RESET);
    if (details && strlen(details) > 0) {
        printf(" %s%s%s", text_color_start, details, ANSI_RESET);
    }
    printf("\n");
    fflush(stdout);
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

typedef struct {
    char *output;
    size_t size;
} MemoryBuffer;

// ============================================================================
// ESC Key Interrupt Handling
// ============================================================================

// Global interrupt flag - set to 1 when ESC is pressed
static volatile sig_atomic_t interrupt_requested = 0;



// Check for ESC key press without blocking
// Returns: 1 if ESC was pressed, 0 otherwise
static int check_for_esc(void) {
    struct termios old_term, new_term;
    int esc_pressed = 0;

    // Save current terminal settings
    if (tcgetattr(STDIN_FILENO, &old_term) < 0) {
        return 0;  // Can't check, assume no ESC
    }

    // Set terminal to non-blocking mode
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);
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
#else
#define STATIC static
#endif

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;

    char *ptr = realloc(mem->output, mem->size + realsize + 1);
    if (!ptr) {
        LOG_ERROR("Not enough memory (realloc returned NULL)");
        return 0;
    }

    mem->output = ptr;
    memcpy(&(mem->output[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->output[mem->size] = 0;

    return realsize;
}

STATIC char* read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = malloc(fsize + 1);
    if (content) {
        fread(content, 1, fsize, f);
        content[fsize] = 0;
    }

    fclose(f);
    return content;
}

STATIC int write_file(const char *path, const char *content) {
    // Create parent directories if they don't exist
    char *path_copy = strdup(path);
    if (!path_copy) return -1;
    
    // Extract directory path
    char *dir_path = dirname(path_copy);
    
    // Create directory recursively (ignore errors if directory already exists)
    char mkdir_cmd[PATH_MAX];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s' 2>/dev/null", dir_path);
    system(mkdir_cmd);
    
    free(path_copy);
    
    // Now try to open/create the file
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    return (written == len) ? 0 : -1;
}

static char* resolve_path(const char *path, const char *working_dir) {
    char *resolved = malloc(PATH_MAX);
    if (!resolved) return NULL;

    if (path[0] == '/') {
        strncpy(resolved, path, PATH_MAX);
    } else {
        snprintf(resolved, PATH_MAX, "%s/%s", working_dir, path);
    }

    // Realpath for cleaning up the path
    char *clean = realpath(resolved, NULL);
    free(resolved);
    return clean;
}

// Add a directory to the additional working directories list
// Returns: 0 on success, -1 on error
int add_directory(ConversationState *state, const char *path) {
    // Validate that directory exists
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
        return -1;  // Path doesn't exist or can't be resolved
    }

    if (stat(resolved_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        free(resolved_path);
        return -1;  // Not a directory
    }

    // Check if directory is already in the list (avoid duplicates)
    if (strcmp(resolved_path, state->working_dir) == 0) {
        free(resolved_path);
        return -1;  // Already the main working directory
    }

    for (int i = 0; i < state->additional_dirs_count; i++) {
        if (strcmp(resolved_path, state->additional_dirs[i]) == 0) {
            free(resolved_path);
            return -1;  // Already in additional directories
        }
    }

    // Expand array if needed
    if (state->additional_dirs_count >= state->additional_dirs_capacity) {
        int new_capacity = state->additional_dirs_capacity == 0 ? 4 : state->additional_dirs_capacity * 2;
        char **new_array = realloc(state->additional_dirs, new_capacity * sizeof(char*));
        if (!new_array) {
            free(resolved_path);
            return -1;  // Out of memory
        }
        state->additional_dirs = new_array;
        state->additional_dirs_capacity = new_capacity;
    }

    // Add directory to list
    state->additional_dirs[state->additional_dirs_count++] = resolved_path;

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
                int line_len = pos - line_start + 1;  // Include the newline
                
                // Check if this line should be included
                int include = 1;
                if (start_line > 0 && current_line < start_line) include = 0;
                if (end_line > 0 && current_line > end_line) include = 0;
                
                if (include) {
                    // Add this line to result
                    char *new_buffer = realloc(result_buffer, result_size + line_len + 1);
                    if (!new_buffer) {
                        free(result_buffer);
                        free(content);
                        cJSON *error = cJSON_CreateObject();
                        cJSON_AddStringToObject(error, "error", "Out of memory");
                        return error;
                    }
                    result_buffer = new_buffer;
                    memcpy(result_buffer + result_size, line_start, line_len);
                    result_size += line_len;
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
            int line_len = strlen(line_start);
            char *new_buffer = realloc(result_buffer, result_size + line_len + 1);
            if (!new_buffer) {
                free(result_buffer);
                free(content);
                cJSON *error = cJSON_CreateObject();
                cJSON_AddStringToObject(error, "error", "Out of memory");
                return error;
            }
            result_buffer = new_buffer;
            memcpy(result_buffer + result_size, line_start, line_len);
            result_size += line_len;
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

static cJSON* tool_write(cJSON *params, ConversationState *state) {
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

    char *resolved_path = resolve_path(path_json->valuestring, state->working_dir);
    if (!resolved_path) {
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "error", "Failed to resolve path");
        return error;
    }

    int ret = write_file(resolved_path, content_json->valuestring);
    free(resolved_path);

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
    ContentBlock *result_block;   // pointer to results array slot
} ToolThreadArg;

static void *tool_thread_func(void *arg) {
    ToolThreadArg *t = (ToolThreadArg *)arg;
    // Execute the tool
    cJSON *res = execute_tool(t->tool_name, t->input, t->state);
    // Free input JSON
    cJSON_Delete(t->input);
    // Populate result block
    t->result_block->type = CONTENT_TOOL_RESULT;
    t->result_block->tool_use_id = t->tool_use_id;
    t->result_block->tool_name = strdup(t->tool_name);  // Store tool name for error reporting
    t->result_block->tool_result = res;
    t->result_block->is_error = cJSON_HasObjectItem(res, "error");
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
    size_t result_len = content_len + (*replace_count) * (new_len - old_len);

    char *result = malloc(result_len + 1);
    if (!result) return NULL;

    char *dest = result;
    const char *src = content;

    while ((pos = strstr(src, old_str)) != NULL) {
        size_t len = pos - src;
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
        size_t prefix_len = match.rm_so;
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
            size_t offset = pos - content;

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

    free(content);
    free(new_content);
    free(resolved_path);

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

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "success");
    cJSON_AddNumberToObject(result, "added", added);
    cJSON_AddNumberToObject(result, "total", total);

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
static void add_cache_control(cJSON *obj);

static cJSON* get_tool_definitions(int enable_caching) {
    cJSON *tool_array = cJSON_CreateArray();

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
static void add_cache_control(cJSON *obj) {
    cJSON *cache_ctrl = cJSON_CreateObject();
    cJSON_AddStringToObject(cache_ctrl, "type", "ephemeral");
    cJSON_AddItemToObject(obj, "cache_control", cache_ctrl);
}

static cJSON* call_api(ConversationState *state) {
    int retry_count = 0;
    int backoff_ms = INITIAL_BACKOFF_MS;

    // Overall API call timing
    struct timespec call_start, call_end;
    clock_gettime(CLOCK_MONOTONIC, &call_start);

    // Debug: Log API URL to detect corruption
    LOG_DEBUG("call_api: api_url=%s", state->api_url ? state->api_url : "(NULL)");

    // Check if prompt caching is enabled
    int enable_caching = is_prompt_caching_enabled();
    LOG_DEBUG("Prompt caching: %s", enable_caching ? "enabled" : "disabled");

    // Time request building
    struct timespec build_start, build_end;
    clock_gettime(CLOCK_MONOTONIC, &build_start);

    // Build request body once (used for all retries)
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "model", state->model);
    cJSON_AddNumberToObject(request, "max_completion_tokens", MAX_TOKENS);

    // Add messages in OpenAI format
    cJSON *messages_array = cJSON_CreateArray();
    for (int i = 0; i < state->count; i++) {
        cJSON *msg = cJSON_CreateObject();

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
                state->messages[i].content[0].type == CONTENT_TEXT) {

                // For system messages, use content array to support cache_control
                cJSON *content_array = cJSON_CreateArray();
                cJSON *text_block = cJSON_CreateObject();
                cJSON_AddStringToObject(text_block, "type", "text");
                cJSON_AddStringToObject(text_block, "text", state->messages[i].content[0].text);

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
                if (state->messages[i].content[j].type == CONTENT_TOOL_RESULT) {
                    has_tool_results = 1;
                    break;
                }
            }

            if (has_tool_results) {
                // For tool results, we need to add them as "tool" role messages
                for (int j = 0; j < state->messages[i].content_count; j++) {
                    ContentBlock *cb = &state->messages[i].content[j];
                    if (cb->type == CONTENT_TOOL_RESULT) {
                        cJSON *tool_msg = cJSON_CreateObject();
                        cJSON_AddStringToObject(tool_msg, "role", "tool");
                        cJSON_AddStringToObject(tool_msg, "tool_call_id", cb->tool_use_id);
                        // Convert result to string
                        char *result_str = cJSON_PrintUnformatted(cb->tool_result);
                        cJSON_AddStringToObject(tool_msg, "content", result_str);
                        free(result_str);
                        cJSON_AddItemToArray(messages_array, tool_msg);
                    }
                }
                continue; // Skip adding the user message itself
            } else {
                // Regular user text message
                if (state->messages[i].content_count > 0 &&
                    state->messages[i].content[0].type == CONTENT_TEXT) {

                    // Use content array for recent messages to support cache_control
                    if (is_recent_message) {
                        cJSON *content_array = cJSON_CreateArray();
                        cJSON *text_block = cJSON_CreateObject();
                        cJSON_AddStringToObject(text_block, "type", "text");
                        cJSON_AddStringToObject(text_block, "text", state->messages[i].content[0].text);

                        // Add cache_control to the last user message
                        if (i == state->count - 1) {
                            add_cache_control(text_block);
                        }

                        cJSON_AddItemToArray(content_array, text_block);
                        cJSON_AddItemToObject(msg, "content", content_array);
                    } else {
                        // For older messages, use simple string content
                        cJSON_AddStringToObject(msg, "content", state->messages[i].content[0].text);
                    }
                }
            }
        } else {
            // Assistant messages
            cJSON *tool_calls = NULL;
            char *text_content = NULL;

            for (int j = 0; j < state->messages[i].content_count; j++) {
                ContentBlock *cb = &state->messages[i].content[j];

                if (cb->type == CONTENT_TEXT) {
                    text_content = cb->text;
                } else if (cb->type == CONTENT_TOOL_USE) {
                    if (!tool_calls) {
                        tool_calls = cJSON_CreateArray();
                    }
                    cJSON *tool_call = cJSON_CreateObject();
                    cJSON_AddStringToObject(tool_call, "id", cb->tool_use_id);
                    cJSON_AddStringToObject(tool_call, "type", "function");
                    cJSON *function = cJSON_CreateObject();
                    cJSON_AddStringToObject(function, "name", cb->tool_name);
                    char *args_str = cJSON_PrintUnformatted(cb->tool_input);
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

    char *json_str = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    clock_gettime(CLOCK_MONOTONIC, &build_end);
    long build_ms = (build_end.tv_sec - build_start.tv_sec) * 1000 +
                    (build_end.tv_nsec - build_start.tv_nsec) / 1000000;
    LOG_INFO("Request building took %ld ms (message count: %d, request size: %zu bytes)",
             build_ms, state->count, strlen(json_str));

    // Keep copy of request for persistence logging
    char *request_copy = strdup(json_str);

    // Validate API URL before proceeding
    if (!state->api_url || state->api_url[0] == '\0') {
        LOG_ERROR("API URL is not set or has been corrupted");
        print_error("Internal error: API URL is missing or corrupted");
        free(request_copy);
        free(json_str);
        return NULL;
    }

    // Build full API URL by appending endpoint to base
    char full_url[512];
    snprintf(full_url, sizeof(full_url), "%s/v1/chat/completions", state->api_url);

    // Retry loop for handling rate limits
    while (retry_count <= MAX_RETRIES) {
        CURL *curl;
        CURLcode res;
        MemoryBuffer response = {NULL, 0};

        curl = curl_easy_init();
        if (!curl) {
            LOG_ERROR("Failed to initialize CURL");
            free(json_str);
            free(request_copy);
            return NULL;
        }

        // Set up headers for OpenAI-compatible API
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", state->api_key);
        headers = curl_slist_append(headers, auth_header);

        // Configure CURL
        curl_easy_setopt(curl, CURLOPT_URL, full_url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        // Time the API call
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        // Check for ESC before making request
        if (check_for_esc()) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            free(request_copy);
            free(json_str);
            LOG_INFO("API call interrupted by user (ESC pressed)");
            return NULL;
        }

        LOG_DEBUG("Starting HTTP request to %s (retry %d/%d)", full_url, retry_count, MAX_RETRIES);

        // Perform request
        res = curl_easy_perform(curl);

        clock_gettime(CLOCK_MONOTONIC, &end);
        long duration_ms = (end.tv_sec - start.tv_sec) * 1000 +
                           (end.tv_nsec - start.tv_nsec) / 1000000;

        LOG_INFO("HTTP request completed in %ld ms (response size: %zu bytes)",
                 duration_ms, response.size);

        // Get HTTP status code
        long http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        // Handle CURL errors
        if (res != CURLE_OK) {
            const char *error_msg = curl_easy_strerror(res);
            LOG_ERROR("CURL request failed: %s", error_msg);

            // Log failed request to persistence
            if (state->persistence_db) {
                persistence_log_api_call(
                    state->persistence_db,
                    state->session_id,
                    state->api_url,
                    request_copy,
                    NULL,  // No response on error
                    state->model,
                    "error",
                    (int)http_status,
                    error_msg,
                    duration_ms,
                    0  // No tools on error
                );
            }

            free(request_copy);
            free(json_str);
            free(response.output);
            return NULL;
        }

        // Parse response
        struct timespec parse_start, parse_end;
        clock_gettime(CLOCK_MONOTONIC, &parse_start);

        cJSON *json_response = cJSON_Parse(response.output);

        clock_gettime(CLOCK_MONOTONIC, &parse_end);
        long parse_ms = (parse_end.tv_sec - parse_start.tv_sec) * 1000 +
                        (parse_end.tv_nsec - parse_start.tv_nsec) / 1000000;
        LOG_INFO("JSON parsing took %ld ms", parse_ms);

        if (!json_response) {
            LOG_ERROR("Failed to parse JSON response");

            // Log parsing error
            if (state->persistence_db) {
                persistence_log_api_call(
                    state->persistence_db,
                    state->session_id,
                    state->api_url,
                    request_copy,
                    response.output,
                    state->model,
                    "error",
                    (int)http_status,
                    "Failed to parse JSON response",
                    duration_ms,
                    0
                );
            }

            free(request_copy);
            free(json_str);
            free(response.output);
            return NULL;
        }

        // Check for API error response
        cJSON *error = cJSON_GetObjectItem(json_response, "error");
        if (error) {
            // Extract error details
            cJSON *error_message = cJSON_GetObjectItem(error, "message");
            cJSON *error_code = cJSON_GetObjectItem(error, "code");

            const char *err_msg = error_message && cJSON_IsString(error_message)
                                  ? error_message->valuestring
                                  : "Unknown error";
            const char *err_code = error_code && cJSON_IsString(error_code)
                                   ? error_code->valuestring
                                   : "";

            // Check if this is a rate limit error (429)
            int is_rate_limit = (http_status == 429 || strcmp(err_code, "429") == 0);

            if (is_rate_limit && retry_count < MAX_RETRIES) {
                // Retry with exponential backoff
                char retry_msg[256];
                snprintf(retry_msg, sizeof(retry_msg), "Rate limit exceeded. Retrying in %d ms...", backoff_ms);
                print_error(retry_msg);

                // Log this attempt
                if (state->persistence_db) {
                    persistence_log_api_call(
                        state->persistence_db,
                        state->session_id,
                        state->api_url,
                        request_copy,
                        response.output,
                        state->model,
                        "error",
                        (int)http_status,
                        err_msg,
                        duration_ms,
                        0
                    );
                }

                // Sleep and retry
                usleep(backoff_ms * 1000); // usleep takes microseconds
                backoff_ms = (int)(backoff_ms * BACKOFF_MULTIPLIER);
                if (backoff_ms > MAX_BACKOFF_MS) {
                    backoff_ms = MAX_BACKOFF_MS;
                }
                retry_count++;

                cJSON_Delete(json_response);
                free(response.output);
                continue; // Retry the request
            }

            // Non-retryable error or max retries exceeded
            char error_msg[512];
            snprintf(error_msg, sizeof(error_msg), "API error: %s", err_msg);
            print_error(error_msg);

            // Log error
            if (state->persistence_db) {
                persistence_log_api_call(
                    state->persistence_db,
                    state->session_id,
                    state->api_url,
                    request_copy,
                    response.output,
                    state->model,
                    "error",
                    (int)http_status,
                    err_msg,
                    duration_ms,
                    0
                );
            }

            cJSON_Delete(json_response);
            free(request_copy);
            free(json_str);
            free(response.output);
            return NULL;
        }

        // Check for valid response format (must have "choices")
        cJSON *choices = cJSON_GetObjectItem(json_response, "choices");
        if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
            print_error("Invalid response format: no choices");

            // Log error
            if (state->persistence_db) {
                persistence_log_api_call(
                    state->persistence_db,
                    state->session_id,
                    state->api_url,
                    request_copy,
                    response.output,
                    state->model,
                    "error",
                    (int)http_status,
                    "Invalid response format: no choices",
                    duration_ms,
                    0
                );
            }

            cJSON_Delete(json_response);
            free(request_copy);
            free(json_str);
            free(response.output);
            return NULL;
        }

        // Count tool uses in response
        int tool_count = 0;
        cJSON *first_choice = cJSON_GetArrayItem(choices, 0);
        if (first_choice) {
            cJSON *message = cJSON_GetObjectItem(first_choice, "message");
            if (message) {
                cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                if (tool_calls && cJSON_IsArray(tool_calls)) {
                    tool_count = cJSON_GetArraySize(tool_calls);
                }
            }
        }

        // Log successful request to persistence
        if (state->persistence_db) {
            persistence_log_api_call(
                state->persistence_db,
                state->session_id,
                state->api_url,
                request_copy,
                response.output,
                state->model,
                "success",
                (int)http_status,
                NULL,  // No error message
                duration_ms,
                tool_count
            );
        }

        free(request_copy);
        free(json_str);
        free(response.output);

        // Log total API call duration
        clock_gettime(CLOCK_MONOTONIC, &call_end);
        long total_ms = (call_end.tv_sec - call_start.tv_sec) * 1000 +
                        (call_end.tv_nsec - call_start.tv_nsec) / 1000000;
        LOG_INFO("Total API call took %ld ms (build: %ld ms, HTTP: %ld ms, parse: %ld ms, tools: %d)",
                 total_ms, build_ms, duration_ms, parse_ms, tool_count);

        return json_response;
    }

    // Max retries exceeded (shouldn't reach here normally)
    free(request_copy);
    free(json_str);
    LOG_ERROR("API call failed after %d retries", MAX_RETRIES);
    return NULL;
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
                offset += snprintf(prompt + offset, prompt_size - offset, ", ");
            }
            offset += snprintf(prompt + offset, prompt_size - offset, "%s", state->additional_dirs[i]);
        }
    }
    offset += snprintf(prompt + offset, prompt_size - offset, "\n");

    offset += snprintf(prompt + offset, prompt_size - offset,
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
        offset += snprintf(prompt + offset, prompt_size - offset, "\n%s\n", git_status);
    }

    // Add CLAUDE.md content if available
    if (claude_md && offset < (int)prompt_size) {
        offset += snprintf(prompt + offset, prompt_size - offset,
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

static void add_system_message(ConversationState *state, const char *text) {
    if (state->count >= MAX_MESSAGES) {
        LOG_ERROR("Maximum message count reached");
        return;
    }

    Message *msg = &state->messages[state->count++];
    msg->role = MSG_SYSTEM;
    msg->content = calloc(1, sizeof(ContentBlock));
    if (!msg->content) {
        LOG_ERROR("Failed to allocate memory for message content");
        state->count--;
        return;
    }
    msg->content_count = 1;
    // calloc already zeros memory, but explicitly set for analyzer
    msg->content[0].type = CONTENT_TEXT;
    msg->content[0].text = NULL;
    msg->content[0].tool_use_id = NULL;
    msg->content[0].tool_name = NULL;
    msg->content[0].tool_input = NULL;
    msg->content[0].tool_result = NULL;

    msg->content[0].text = strdup(text);
    if (!msg->content[0].text) {
        LOG_ERROR("Failed to duplicate message text");
        free(msg->content);
        msg->content = NULL;
        state->count--;
        return;
    }
}

static void add_user_message(ConversationState *state, const char *text) {
    if (state->count >= MAX_MESSAGES) {
        LOG_ERROR("Maximum message count reached");
        return;
    }

    Message *msg = &state->messages[state->count++];
    msg->role = MSG_USER;
    msg->content = calloc(1, sizeof(ContentBlock));
    if (!msg->content) {
        LOG_ERROR("Failed to allocate memory for message content");
        state->count--; // Rollback count increment
        return;
    }
    msg->content_count = 1;
    // calloc already zeros memory, but explicitly set for analyzer
    msg->content[0].type = CONTENT_TEXT;
    msg->content[0].text = NULL;
    msg->content[0].tool_use_id = NULL;
    msg->content[0].tool_name = NULL;
    msg->content[0].tool_input = NULL;
    msg->content[0].tool_result = NULL;

    msg->content[0].text = strdup(text);
    if (!msg->content[0].text) {
        LOG_ERROR("Failed to duplicate message text");
        free(msg->content);
        msg->content = NULL;
        state->count--; // Rollback count increment
        return;
    }
}

// Parse OpenAI message format and add to conversation
static void add_assistant_message_openai(ConversationState *state, cJSON *message) {
    if (state->count >= MAX_MESSAGES) {
        LOG_ERROR("Maximum message count reached");
        return;
    }

    Message *msg = &state->messages[state->count++];
    msg->role = MSG_ASSISTANT;

    // Count content blocks (text + tool calls)
    int content_count = 0;
    cJSON *content = cJSON_GetObjectItem(message, "content");
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");

    if (content && cJSON_IsString(content) && content->valuestring) {
        content_count++;
    }

    int tool_calls_count = 0;
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        tool_calls_count = cJSON_GetArraySize(tool_calls);
        content_count += tool_calls_count;
    }

    // Ensure we have at least some content
    if (content_count == 0) {
        LOG_WARN("Assistant message has no content");
        state->count--; // Rollback count increment
        return;
    }

    msg->content = calloc(content_count, sizeof(ContentBlock));
    if (!msg->content) {
        LOG_ERROR("Failed to allocate memory for message content");
        state->count--; // Rollback count increment
        return;
    }
    msg->content_count = content_count;

    int idx = 0;

    // Add text content if present
    if (content && cJSON_IsString(content) && content->valuestring) {
        msg->content[idx].type = CONTENT_TEXT;
        msg->content[idx].text = strdup(content->valuestring);
        if (!msg->content[idx].text) {
            LOG_ERROR("Failed to duplicate message text");
            free(msg->content);
            msg->content = NULL;
            state->count--;
            return;
        }
        idx++;
    }

    // Add tool calls if present
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        for (int i = 0; i < tool_calls_count; i++) {
            cJSON *tool_call = cJSON_GetArrayItem(tool_calls, i);
            cJSON *id = cJSON_GetObjectItem(tool_call, "id");
            cJSON *function = cJSON_GetObjectItem(tool_call, "function");

            if (function) {
                cJSON *name = cJSON_GetObjectItem(function, "name");
                cJSON *arguments = cJSON_GetObjectItem(function, "arguments");

                msg->content[idx].type = CONTENT_TOOL_USE;
                msg->content[idx].tool_use_id = strdup(id->valuestring);
                if (!msg->content[idx].tool_use_id) {
                    LOG_ERROR("Failed to duplicate tool use ID");
                    // Cleanup previously allocated content
                    for (int j = 0; j < idx; j++) {
                        free(msg->content[j].text);
                        free(msg->content[j].tool_use_id);
                        free(msg->content[j].tool_name);
                    }
                    free(msg->content);
                    msg->content = NULL;
                    state->count--;
                    return;
                }
                msg->content[idx].tool_name = strdup(name->valuestring);
                if (!msg->content[idx].tool_name) {
                    LOG_ERROR("Failed to duplicate tool name");
                    free(msg->content[idx].tool_use_id);
                    // Cleanup previously allocated content
                    for (int j = 0; j < idx; j++) {
                        free(msg->content[j].text);
                        free(msg->content[j].tool_use_id);
                        free(msg->content[j].tool_name);
                    }
                    free(msg->content);
                    msg->content = NULL;
                    state->count--;
                    return;
                }

                // Parse arguments string as JSON
                if (arguments && cJSON_IsString(arguments)) {
                    msg->content[idx].tool_input = cJSON_Parse(arguments->valuestring);
                } else {
                    msg->content[idx].tool_input = cJSON_CreateObject();
                }
                idx++;
            }
        }
    }
}

static void add_tool_results(ConversationState *state, ContentBlock *results, int count) {
    if (state->count >= MAX_MESSAGES) {
        LOG_ERROR("Maximum message count reached");
        return;
    }

    Message *msg = &state->messages[state->count++];
    msg->role = MSG_USER;
    msg->content = results;
    msg->content_count = count;
}

// ============================================================================
// Interactive Mode - Simple Terminal I/O
// ============================================================================

void clear_conversation(ConversationState *state) {
    // Keep the system message (first message)
    int system_msg_count = 0;

    if (state->count > 0 && state->messages[0].role == MSG_SYSTEM) {
        // System message remains intact
        system_msg_count = 1;
    }

    // Free all other message content
    for (int i = system_msg_count; i < state->count; i++) {
        for (int j = 0; j < state->messages[i].content_count; j++) {
            ContentBlock *cb = &state->messages[i].content[j];
            free(cb->text);
            free(cb->tool_use_id);
            free(cb->tool_name);
            if (cb->tool_input) cJSON_Delete(cb->tool_input);
            if (cb->tool_result) cJSON_Delete(cb->tool_result);
        }
        free(state->messages[i].content);
    }

    // Reset message count (keeping system message)
    state->count = system_msg_count;
}

static void process_response(ConversationState *state, cJSON *response, TUIState *tui) {
    // Time the entire response processing
    struct timespec proc_start, proc_end;
    clock_gettime(CLOCK_MONOTONIC, &proc_start);

    // OpenAI response format
    cJSON *choices = cJSON_GetObjectItem(response, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        if (tui) {
            tui_add_conversation_line(tui, "[Error]", "Invalid response format: no choices", COLOR_PAIR_ERROR);
        } else {
            print_error("Invalid response format: no choices");
        }
        return;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!message) {
        if (tui) {
            tui_add_conversation_line(tui, "[Error]", "Invalid response format: no message", COLOR_PAIR_ERROR);
        } else {
            print_error("Invalid response format: no message");
        }
        return;
    }

    // Display assistant's text content if present
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content) && content->valuestring) {
        // Skip whitespace-only content
        const char *p = content->valuestring;
        while (*p && isspace((unsigned char)*p)) p++;

        if (*p != '\0') {  // Has non-whitespace content
            if (tui) {
                tui_add_conversation_line(tui, "[Assistant]", content->valuestring, COLOR_PAIR_ASSISTANT);
            } else {
                print_assistant(content->valuestring);
            }
        }
    }

    // Add to conversation history
    add_assistant_message_openai(state, message);

    // Check for tool calls
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    int tool_count = 0;
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        tool_count = cJSON_GetArraySize(tool_calls);
    }

    if (tool_count > 0) {
        if (1) { // Force non-TUI spinner path for testing
            printf("\n");
        }

        LOG_INFO("Processing %d tool call(s)", tool_count);

        // Time tool execution phase
        struct timespec tool_start, tool_end;
        clock_gettime(CLOCK_MONOTONIC, &tool_start);

        // Parallel tool execution
        ContentBlock *results = calloc(tool_count, sizeof(ContentBlock));
        pthread_t *threads = malloc(tool_count * sizeof(pthread_t));
        ToolThreadArg *args = malloc(tool_count * sizeof(ToolThreadArg));
        int thread_count = 0;

        // Launch tool execution threads
        for (int i = 0; i < tool_count; i++) {
            cJSON *tool_call = cJSON_GetArrayItem(tool_calls, i);
            cJSON *id = cJSON_GetObjectItem(tool_call, "id");
            cJSON *function = cJSON_GetObjectItem(tool_call, "function");
            if (!function) {
                continue;
            }
            cJSON *name = cJSON_GetObjectItem(function, "name");
            cJSON *arguments = cJSON_GetObjectItem(function, "arguments");

            // Parse arguments to get details
            cJSON *input = NULL;
            if (arguments && cJSON_IsString(arguments)) {
                input = cJSON_Parse(arguments->valuestring);
            } else {
                input = cJSON_CreateObject();
            }
            
            char *tool_details = get_tool_details(name->valuestring, input);
            
            if (tui) {
                char prefix_with_tool[128];
                snprintf(prefix_with_tool, sizeof(prefix_with_tool), "[Tool: %s]", name->valuestring);
                
                // Pass NULL for text when there are no details to avoid extra space
                tui_add_conversation_line(tui, prefix_with_tool, tool_details, COLOR_PAIR_TOOL);
            } else {
                print_tool(name->valuestring, tool_details);
            }

            // Prepare thread arguments (input already parsed above)
            args[thread_count].tool_use_id = strdup(id->valuestring);
            args[thread_count].tool_name = name->valuestring;
            args[thread_count].input = input;
            args[thread_count].state = state;
            args[thread_count].result_block = &results[i];

            // Create thread
            pthread_create(&threads[thread_count], NULL, tool_thread_func, &args[thread_count]);
            thread_count++;
        }

        // Show spinner or status while waiting for tools to complete
        Spinner *tool_spinner = NULL;
        if (!tui) {
            char spinner_msg[128];
            snprintf(spinner_msg, sizeof(spinner_msg), "Running %d tool%s...",
                     thread_count, thread_count > 1 ? "s" : "");
            tool_spinner = spinner_start(spinner_msg, SPINNER_YELLOW);
        } else {
            char status_msg[128];
            snprintf(status_msg, sizeof(status_msg), "Running %d tool%s...",
                     thread_count, thread_count > 1 ? "s" : "");
            tui_update_status(tui, status_msg);
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
                LOG_INFO("Tool execution interrupted by user (ESC pressed)");
                interrupted = 1;

                // Notify user immediately
                if (!tui) {
                    spinner_stop(tool_spinner, "Interrupted by user (ESC) - waiting for tools to finish...", 0);
                    tool_spinner = NULL;  // Mark as stopped
                } else {
                    tui_update_status(tui, "Interrupted by user (ESC) - waiting for tools to finish...");
                }
                break;
            }
            usleep(50000);  // Check every 50ms
        }

        // Wait for monitor thread to complete (this ensures all tool threads are joined)
        pthread_join(monitor_thread, NULL);

        clock_gettime(CLOCK_MONOTONIC, &tool_end);
        long tool_exec_ms = (tool_end.tv_sec - tool_start.tv_sec) * 1000 +
                            (tool_end.tv_nsec - tool_start.tv_nsec) / 1000000;
        LOG_INFO("All %d tool(s) completed in %ld ms", thread_count, tool_exec_ms);

        // If interrupted, clean up and return
        if (interrupted) {
            if (!tui) {
                if (tool_spinner) {
                    spinner_stop(tool_spinner, "Interrupted by user (ESC)", 0);
                }
            } else {
                tui_update_status(tui, "Interrupted by user (ESC)");
            }

            free(threads);
            free(args);
            free(results);
            return;  // Exit without continuing conversation
        }

        // Check if any tools had errors and display error messages
        int has_error = 0;
        for (int i = 0; i < tool_count; i++) {
            if (results[i].is_error) {
                has_error = 1;

                // Extract and display the error message
                cJSON *error_obj = cJSON_GetObjectItem(results[i].tool_result, "error");
                const char *error_msg = error_obj && cJSON_IsString(error_obj)
                    ? error_obj->valuestring
                    : "Unknown error";

                // Get tool name for better context
                const char *tool_name = results[i].tool_name ? results[i].tool_name : "tool";

                // Display error next to/below the tool execution
                if (tui) {
                    char error_display[512];
                    snprintf(error_display, sizeof(error_display), "%s failed: %s", tool_name, error_msg);
                    tui_add_conversation_line(tui, "[Error]", error_display, COLOR_PAIR_ERROR);
                } else {
                    char error_display[512];
                    snprintf(error_display, sizeof(error_display), "[Error] %s failed: %s", tool_name, error_msg);
                    // Try to get color from colorscheme, fall back to centralized ANSI color
                    char color_code[32];
                    const char *color_start;
                    if (get_colorscheme_color(COLORSCHEME_ERROR, color_code, sizeof(color_code)) == 0) {
                        color_start = color_code;
                    } else {
                        color_start = ANSI_FALLBACK_ERROR;
                    }
                    printf("%s%s%s\n", color_start, error_display, ANSI_RESET);
                    fflush(stdout);
                }
            }
        }

        // Clear status on success, show message on error
        if (!tui) {
            printf("DEBUG: has_error = %d\n", has_error);
            if (has_error) {
                spinner_stop(tool_spinner, "Tool execution completed with errors", 0);
            } else {
                printf("DEBUG: Stopping tool spinner with success message\n");
                spinner_stop(tool_spinner, "Tool execution completed successfully", 1);
            }
        } else {
            if (has_error) {
                tui_update_status(tui, "Tool execution completed with errors");
            } else {
                tui_update_status(tui, "");  // Clear status on success
            }
        }
        free(threads);
        free(args);

        // Add tool results and continue conversation
        add_tool_results(state, results, tool_count);

        // Debug: Verify API URL is still valid after tool execution
        LOG_DEBUG("After tool execution: api_url=%s", state->api_url ? state->api_url : "(NULL)");
        if (!state->api_url || state->api_url[0] == '\0') {
            LOG_ERROR("API URL corrupted after tool execution!");
            const char *error_msg = "Internal error: API URL was corrupted during tool execution";
            if (tui) {
                tui_add_conversation_line(tui, "[Error]", error_msg, COLOR_PAIR_ERROR);
            } else {
                print_error(error_msg);
            }
            free(results);
            return;
        }

        Spinner *followup_spinner = NULL;
        if (!tui) {
            followup_spinner = spinner_start("Processing tool results...", SPINNER_CYAN);
        } else {
            tui_update_status(tui, "Processing tool results...");
        }
        cJSON *next_response = call_api(state);
        // Clear status after API call completes
        if (!tui) {
            spinner_stop(followup_spinner, NULL, 1);  // Clear without message
        } else {
            tui_update_status(tui, "");  // Clear status
        }
        if (next_response) {
            process_response(state, next_response, tui);
            cJSON_Delete(next_response);
        } else {
            // API call failed - show error to user
            const char *error_msg = "API call failed after executing tools. Check logs for details.";
            if (tui) {
                tui_add_conversation_line(tui, "[Error]", error_msg, COLOR_PAIR_ERROR);
            } else {
                print_error(error_msg);
            }
            LOG_ERROR("API call returned NULL after tool execution");
        }

        // Free results array; content of results will be freed in cleanup
        free(results);

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

// ============================================================================
// Advanced Input Handler (readline-like)
// ============================================================================













// Advanced input handler with readline-like keybindings
static void interactive_mode(ConversationState *state) {
    // Initialize colorscheme FIRST (before any colored output)
    const char *theme = getenv("CLAUDE_C_THEME");
    if (theme && strlen(theme) > 0) {
        char theme_path[512];
        snprintf(theme_path, sizeof(theme_path), "colorschemes/%s.conf", theme);
        if (init_colorscheme(theme_path) != 0) {
            LOG_WARN("Failed to load colorscheme '%s', using default", theme);
        }
    } else {
        // Try to load default theme
        if (init_colorscheme("colorschemes/kitty-default.conf") != 0) {
            LOG_WARN("Failed to load default colorscheme");
        }
    }

    // Display startup banner with theme colors
    char color_code[32];
    const char *banner_color;
    if (get_colorscheme_color(COLORSCHEME_ASSISTANT, color_code, sizeof(color_code)) == 0) {
        banner_color = color_code;
    } else {
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

    int running = 1;

    while (running) {
        char *input = tui_read_input(&tui, ">");
        if (!input) {
            // EOF (Ctrl+D)
            break;
        }

        // Skip empty input
        if (strlen(input) == 0) {
            free(input);
            continue;
        }

        // Handle commands
        if (input[0] == '/') {
            // Show command in conversation
            tui_add_conversation_line(&tui, "[User]", input, COLOR_PAIR_USER);

            if (strcmp(input, "/exit") == 0 || strcmp(input, "/quit") == 0) {
                free(input);
                break;
            } else if (strcmp(input, "/clear") == 0) {
                clear_conversation(state);
                tui_clear_conversation(&tui);
                tui_add_conversation_line(&tui, "[System]", "Conversation cleared", COLOR_PAIR_STATUS);
                free(input);
                continue;
            } else if (strncmp(input, "/add-dir ", 9) == 0) {
                const char *path = input + 9;
                if (add_directory(state, path) == 0) {
                    tui_add_conversation_line(&tui, "[System]", "Directory added successfully", COLOR_PAIR_STATUS);
                    // Update system message
                    char *new_system_prompt = build_system_prompt(state);
                    if (new_system_prompt) {
                        if (state->count > 0 && state->messages[0].role == MSG_SYSTEM) {
                            free(state->messages[0].content[0].text);
                            state->messages[0].content[0].text = new_system_prompt;
                        }
                    }
                } else {
                    tui_add_conversation_line(&tui, "[Error]", "Failed to add directory", COLOR_PAIR_ERROR);
                }
                free(input);
                continue;
            } else if (strcmp(input, "/help") == 0) {
                tui_add_conversation_line(&tui, "[System]", "Available commands:", COLOR_PAIR_STATUS);
                tui_add_conversation_line(&tui, "[System]", "  /exit, /quit - Exit the program", COLOR_PAIR_STATUS);
                tui_add_conversation_line(&tui, "[System]", "  /clear - Clear conversation history", COLOR_PAIR_STATUS);
                tui_add_conversation_line(&tui, "[System]", "  /add-dir <path> - Add additional working directory", COLOR_PAIR_STATUS);
                tui_add_conversation_line(&tui, "[System]", "  /help - Show this help message", COLOR_PAIR_STATUS);
                free(input);
                continue;
            } else {
                tui_add_conversation_line(&tui, "[Error]", "Unknown command. Type /help for available commands.", COLOR_PAIR_ERROR);
                free(input);
                continue;
            }
        }

        // Display user message
        tui_add_conversation_line(&tui, "[User]", input, COLOR_PAIR_USER);

        // Add to conversation
        add_user_message(state, input);
        free(input);

        // Call API with status update
        tui_update_status(&tui, "Waiting for API response...");
        cJSON *response = call_api(state);
        tui_update_status(&tui, "");  // Clear status after API response

        if (!response) {
            tui_add_conversation_line(&tui, "[Error]", "Failed to get response from API", COLOR_PAIR_ERROR);
            continue;
        }

        // Check for errors
        cJSON *error = cJSON_GetObjectItem(response, "error");
        if (error) {
            cJSON *error_message = cJSON_GetObjectItem(error, "message");
            const char *error_msg = error_message ? error_message->valuestring : "Unknown error";
            tui_add_conversation_line(&tui, "[Error]", error_msg, COLOR_PAIR_ERROR);
            cJSON_Delete(response);
            continue;
        }

        process_response(state, response, &tui);
        cJSON_Delete(response);
    }

    // Cleanup TUI
    tui_cleanup(&tui);
    printf("Goodbye!\n");
}

// ============================================================================
// Session ID Generation
// ============================================================================

// Generate a unique session ID using timestamp and random data
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
    // Handle help flag first (no API key needed)
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("Claude Code - Pure C Implementation (OpenAI Compatible)\n\n");
        printf("Usage:\n");
        printf("  %s               Start interactive mode\n", argv[0]);
        printf("  %s -h, --help    Show this help message\n\n", argv[0]);
        printf("Environment Variables:\n");
        printf("  OPENAI_API_KEY       Required: Your OpenAI API key\n");
        printf("  OPENAI_API_BASE      Optional: API base URL (default: %s)\n", API_BASE_URL);
        printf("  OPENAI_MODEL         Optional: Model name (default: %s)\n\n", DEFAULT_MODEL);
        return 0;
    }

    // Check that no extra arguments were provided
    if (argc > 1) {
        LOG_ERROR("Unexpected arguments provided");
        return 1;
    }

    // Check for API key
    const char *api_key = getenv("OPENAI_API_KEY");
    if (!api_key) {
        LOG_ERROR("OPENAI_API_KEY environment variable not set");
        return 1;
    }

    // Get optional API base and model from environment
    const char *api_base = getenv("OPENAI_API_BASE");
    if (!api_base) {
        api_base = API_BASE_URL;
    }

    const char *model = getenv("OPENAI_MODEL");
    if (!model) {
        model = DEFAULT_MODEL;
    }

    // Initialize CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);

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

    // Initialize conversation state
    ConversationState state = {0};
    state.api_key = strdup(api_key);
    state.api_url = strdup(api_base);
    state.model = strdup(model);
    state.working_dir = getcwd(NULL, 0);
    state.session_id = session_id;
    state.persistence_db = persistence_db;

    // Check for allocation failures
    if (!state.api_key || !state.api_url || !state.model) {
        LOG_ERROR("Failed to allocate memory for conversation state");
        fprintf(stderr, "Error: Memory allocation failed\n");
        free(state.api_key);
        free(state.api_url);
        free(state.model);
        free(state.working_dir);
        curl_global_cleanup();
        return 1;
    }

    if (!state.working_dir) {
        LOG_ERROR("Failed to get current working directory");
        free(state.api_key);
        free(state.api_url);
        free(state.model);
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

    // Cleanup
    for (int i = 0; i < state.count; i++) {
        if (state.messages[i].content) {
            for (int j = 0; j < state.messages[i].content_count; j++) {
                ContentBlock *cb = &state.messages[i].content[j];
                free(cb->text);
                free(cb->tool_use_id);
                free(cb->tool_name);
                if (cb->tool_input) cJSON_Delete(cb->tool_input);
                if (cb->tool_result) cJSON_Delete(cb->tool_result);
            }
            free(state.messages[i].content);
        }
    }

    // Free additional directories
    for (int i = 0; i < state.additional_dirs_count; i++) {
        free(state.additional_dirs[i]);
    }
    free(state.additional_dirs);

    free(state.api_key);
    free(state.api_url);
    free(state.model);
    free(state.working_dir);
    free(state.session_id);

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
