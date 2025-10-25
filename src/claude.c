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

#ifdef TEST_BUILD
// Test build: stub out logging and persistence
#define LOG_INFO(...) do {} while (0)
#define LOG_WARN(...) do {} while (0)
static int log_init(void) { return 0; }
static void log_set_rotation(int a, int b) { (void)a; (void)b; }
static void log_set_level(int level) { (void)level; }
static void log_shutdown(void) {}
// Stub persistence types and functions
typedef struct PersistenceDB { int dummy; } PersistenceDB;
static PersistenceDB* persistence_init(const char *path) { (void)path; return NULL; }
static void persistence_close(PersistenceDB *db) { (void)db; }
static void persistence_log_api_call(
    PersistenceDB *db,
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

#ifdef TEST_BUILD
#define main claude_main
#endif

#ifdef TEST_BUILD
#define main claude_main
#endif

// Version
#define VERSION "0.0.1"

// Configuration - defaults can be overridden by environment variables
#define API_BASE_URL "https://api.openai.com"
#define DEFAULT_MODEL "o4-mini"
#define MAX_TOKENS 16384
#define MAX_MESSAGES 100
#define MAX_TOOLS 10
#define BUFFER_SIZE 8192

// Retry configuration for rate limiting (429 errors)
#define MAX_RETRIES 3                    // Maximum number of retry attempts
#define INITIAL_BACKOFF_MS 1000          // Initial backoff delay in milliseconds
#define MAX_BACKOFF_MS 10000             // Maximum backoff delay in milliseconds (10 seconds)
#define BACKOFF_MULTIPLIER 2.0           // Exponential backoff multiplier

// ANSI color codes (for non-TUI output)
#define ANSI_RESET "\033[0m"
#define ANSI_BLUE "\033[34m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_RED "\033[31m"
#define ANSI_CYAN "\033[36m"

// ============================================================================
// Output Helpers
// ============================================================================

static void print_user(const char *text) {
    printf("%s[User]%s %s\n", ANSI_GREEN, ANSI_RESET, text);
    fflush(stdout);
}

static void print_assistant(const char *text) {
    printf("%s[Assistant]%s %s\n", ANSI_BLUE, ANSI_RESET, text);
    fflush(stdout);
}

static void print_tool(const char *tool_name) {
    printf("%s[Tool: %s]%s\n", ANSI_YELLOW, tool_name, ANSI_RESET);
    fflush(stdout);
}

static void print_error(const char *text) {
    fprintf(stderr, "%s[Error]%s %s\n", ANSI_RED, ANSI_RESET, text);
    fflush(stderr);
}

static void print_status(const char *text) {
    printf("%s[Status]%s %s\n", ANSI_CYAN, ANSI_RESET, text);
    fflush(stdout);
}

// ============================================================================
// Data Structures
// ============================================================================

typedef enum {
    MSG_USER,
    MSG_ASSISTANT,
    MSG_SYSTEM
} MessageRole;

typedef enum {
    CONTENT_TEXT,
    CONTENT_TOOL_USE,
    CONTENT_TOOL_RESULT
} ContentType;

typedef struct {
    ContentType type;
    char *text;              // For TEXT
    char *tool_use_id;       // For TOOL_USE and TOOL_RESULT
    char *tool_name;         // For TOOL_USE
    cJSON *tool_input;       // For TOOL_USE
    cJSON *tool_result;      // For TOOL_RESULT
    int is_error;            // For TOOL_RESULT
} ContentBlock;

typedef struct {
    MessageRole role;
    ContentBlock *content;
    int content_count;
} Message;

typedef struct {
    Message messages[MAX_MESSAGES];
    int count;
    char *api_key;
    char *api_url;
    char *model;
    char *working_dir;
    char *session_id;               // Unique session identifier for this conversation
    PersistenceDB *persistence_db;  // For logging API calls to SQLite
} ConversationState;

typedef struct {
    char *output;
    size_t size;
} MemoryBuffer;


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
        fprintf(stderr, "Not enough memory (realloc returned NULL)\n");
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
    char full_pattern[PATH_MAX];
    snprintf(full_pattern, sizeof(full_pattern), "%s/%s", state->working_dir, pattern);

    glob_t glob_result;
    int ret = glob(full_pattern, GLOB_TILDE, NULL, &glob_result);

    cJSON *result = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();

    if (ret == 0) {
        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
            cJSON_AddItemToArray(files, cJSON_CreateString(glob_result.gl_pathv[i]));
        }
    }

    cJSON_AddItemToObject(result, "files", files);
    cJSON_AddNumberToObject(result, "count", glob_result.gl_pathc);

    globfree(&glob_result);
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

    // Use grep command for simplicity
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

    cJSON *result = cJSON_CreateObject();
    cJSON *matches = cJSON_CreateArray();

    char buffer[BUFFER_SIZE];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        buffer[strcspn(buffer, "\n")] = 0;  // Remove newline
        cJSON_AddItemToArray(matches, cJSON_CreateString(buffer));
    }

    pclose(pipe);

    cJSON_AddItemToObject(result, "matches", matches);
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
};

static const int num_tools = sizeof(tools) / sizeof(Tool);

static cJSON* execute_tool(const char *tool_name, cJSON *input, ConversationState *state) {
    for (int i = 0; i < num_tools; i++) {
        if (strcmp(tools[i].name, tool_name) == 0) {
            return tools[i].handler(input, state);
        }
    }

    cJSON *error = cJSON_CreateObject();
    cJSON_AddStringToObject(error, "error", "Unknown tool");
    return error;
}

// ============================================================================
// Tool Definitions for API
// ============================================================================

static cJSON* get_tool_definitions() {
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

    return tool_array;
}

// ============================================================================
// API Client
// ============================================================================

static cJSON* call_api(ConversationState *state) {
    int retry_count = 0;
    int backoff_ms = INITIAL_BACKOFF_MS;

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

        // Build content based on message type
        if (state->messages[i].role == MSG_SYSTEM) {
            // System messages: just plain text
            if (state->messages[i].content_count > 0 &&
                state->messages[i].content[0].type == CONTENT_TEXT) {
                cJSON_AddStringToObject(msg, "content", state->messages[i].content[0].text);
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
                    cJSON_AddStringToObject(msg, "content", state->messages[i].content[0].text);
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

    // Add tools
    cJSON *tool_defs = get_tool_definitions();
    cJSON_AddItemToObject(request, "tools", tool_defs);

    char *json_str = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    // Keep copy of request for persistence logging
    char *request_copy = strdup(json_str);

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
            fprintf(stderr, "Failed to initialize CURL\n");
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

        // Perform request
        res = curl_easy_perform(curl);

        clock_gettime(CLOCK_MONOTONIC, &end);
        long duration_ms = (end.tv_sec - start.tv_sec) * 1000 +
                           (end.tv_nsec - start.tv_nsec) / 1000000;

        // Get HTTP status code
        long http_status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        // Handle CURL errors
        if (res != CURLE_OK) {
            const char *error_msg = curl_easy_strerror(res);
            fprintf(stderr, "CURL request failed: %s\n", error_msg);

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
        cJSON *json_response = cJSON_Parse(response.output);
        if (!json_response) {
            fprintf(stderr, "Failed to parse JSON response\n");

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

        return json_response;
    }

    // Max retries exceeded (shouldn't reach here normally)
    free(request_copy);
    free(json_str);
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
        output = realloc(output, output_size + len + 1);
        if (!output) {
            pclose(fp);
            return NULL;
        }
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

// Build complete system prompt with environment context
static char* build_system_prompt(const char *working_dir) {
    char *date = get_current_date();
    const char *platform = get_platform();
    char *os_version = get_os_version();
    int is_git = is_git_repo(working_dir);
    char *git_status = is_git ? get_git_status(working_dir) : NULL;

    // Calculate required buffer size
    size_t prompt_size = 2048; // Base size for the prompt template
    if (git_status) prompt_size += strlen(git_status);

    char *prompt = malloc(prompt_size);
    if (!prompt) {
        free(date);
        free(os_version);
        free(git_status);
        return NULL;
    }

    // Build the system prompt
    int offset = snprintf(prompt, prompt_size,
        "Here is useful information about the environment you are running in:\n"
        "<env>\n"
        "Working directory: %s\n"
        "Is directory a git repo: %s\n"
        "Platform: %s\n"
        "OS Version: %s\n"
        "Today's date: %s\n"
        "</env>\n",
        working_dir,
        is_git ? "Yes" : "No",
        platform,
        os_version,
        date);

    // Add git status if available
    if (git_status && offset < (int)prompt_size) {
        snprintf(prompt + offset, prompt_size - offset, "\n%s\n", git_status);
    }

    free(date);
    free(os_version);
    free(git_status);

    return prompt;
}

// ============================================================================
// Message Management
// ============================================================================

static void add_system_message(ConversationState *state, const char *text) {
    if (state->count >= MAX_MESSAGES) {
        fprintf(stderr, "Maximum message count reached\n");
        return;
    }

    Message *msg = &state->messages[state->count++];
    msg->role = MSG_SYSTEM;
    msg->content = calloc(1, sizeof(ContentBlock));
    msg->content_count = 1;
    msg->content[0].type = CONTENT_TEXT;
    msg->content[0].text = strdup(text);
}

static void add_user_message(ConversationState *state, const char *text) {
    if (state->count >= MAX_MESSAGES) {
        fprintf(stderr, "Maximum message count reached\n");
        return;
    }

    Message *msg = &state->messages[state->count++];
    msg->role = MSG_USER;
    msg->content = calloc(1, sizeof(ContentBlock));
    msg->content_count = 1;
    msg->content[0].type = CONTENT_TEXT;
    msg->content[0].text = strdup(text);
}

// Parse OpenAI message format and add to conversation
static void add_assistant_message_openai(ConversationState *state, cJSON *message) {
    if (state->count >= MAX_MESSAGES) {
        fprintf(stderr, "Maximum message count reached\n");
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

    msg->content = calloc(content_count, sizeof(ContentBlock));
    msg->content_count = content_count;

    int idx = 0;

    // Add text content if present
    if (content && cJSON_IsString(content) && content->valuestring) {
        msg->content[idx].type = CONTENT_TEXT;
        msg->content[idx].text = strdup(content->valuestring);
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
                msg->content[idx].tool_name = strdup(name->valuestring);

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
        fprintf(stderr, "Maximum message count reached\n");
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

static void clear_conversation(ConversationState *state) {
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

static void process_response(ConversationState *state, cJSON *response) {
    // OpenAI response format
    cJSON *choices = cJSON_GetObjectItem(response, "choices");
    if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        print_error("Invalid response format: no choices");
        return;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!message) {
        print_error("Invalid response format: no message");
        return;
    }

    // Display assistant's text content if present
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content) && content->valuestring) {
        print_assistant(content->valuestring);
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
        printf("\n");

        // Parallel tool execution
        ContentBlock *results = calloc(tool_count, sizeof(ContentBlock));
        pthread_t *threads = malloc(tool_count * sizeof(pthread_t));
        ToolThreadArg *args = malloc(tool_count * sizeof(ToolThreadArg));

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

            print_tool(name->valuestring);

            // Prepare thread arguments
            cJSON *input = NULL;
            if (arguments && cJSON_IsString(arguments)) {
                input = cJSON_Parse(arguments->valuestring);
            } else {
                input = cJSON_CreateObject();
            }
            args[i].tool_use_id = strdup(id->valuestring);
            args[i].tool_name = name->valuestring;
            args[i].input = input;
            args[i].state = state;
            args[i].result_block = &results[i];

            // Create thread
            pthread_create(&threads[i], NULL, tool_thread_func, &args[i]);
        }

        // Show spinner while waiting for tools to complete
        char spinner_msg[128];
        snprintf(spinner_msg, sizeof(spinner_msg), "Running %d tool%s...", 
                 tool_count, tool_count > 1 ? "s" : "");
        Spinner *tool_spinner = spinner_start(spinner_msg, SPINNER_YELLOW);

        // Wait for all tool threads to complete
        for (int i = 0; i < tool_count; i++) {
            pthread_join(threads[i], NULL);
        }
        
        spinner_stop(tool_spinner, "Tools completed", 1);
        free(threads);
        free(args);

        // Add tool results and continue conversation
        add_tool_results(state, results, tool_count);

        Spinner *followup_spinner = spinner_start("Processing tool results...", SPINNER_CYAN);
        cJSON *next_response = call_api(state);
        spinner_stop(followup_spinner, NULL, 1);
        if (next_response) {
            process_response(state, next_response);
            cJSON_Delete(next_response);
        }

        // Free results array; content of results will be freed in cleanup
        free(results);
        return;
    }
}

// ============================================================================
// Advanced Input Handler (readline-like)
// ============================================================================

// Helper: Check if character is word boundary
#ifdef TEST_BUILD
int is_word_boundary(char c) {
#else
static int is_word_boundary(char c) {
#endif
    return !isalnum(c) && c != '_';
}

// Helper: Move cursor backward by one word
#ifdef TEST_BUILD
int move_backward_word(const char *buffer, int cursor_pos) {
#else
static int move_backward_word(const char *buffer, int cursor_pos) {
#endif
    if (cursor_pos <= 0) return 0;

    int pos = cursor_pos - 1;

    // Skip trailing whitespace/punctuation
    while (pos > 0 && is_word_boundary(buffer[pos])) {
        pos--;
    }

    // Skip the word characters
    while (pos > 0 && !is_word_boundary(buffer[pos])) {
        pos--;
    }

    // If we stopped at a boundary (not at start), move one forward
    if (pos > 0 && is_word_boundary(buffer[pos])) {
        pos++;
    }

    return pos;
}

// Helper: Move cursor forward by one word
#ifdef TEST_BUILD
int move_forward_word(const char *buffer, int cursor_pos, int buffer_len) {
#else
static int move_forward_word(const char *buffer, int cursor_pos, int buffer_len) {
#endif
    if (cursor_pos >= buffer_len) return buffer_len;

    int pos = cursor_pos;

    // Skip current word characters
    while (pos < buffer_len && !is_word_boundary(buffer[pos])) {
        pos++;
    }

    // Skip trailing whitespace/punctuation
    while (pos < buffer_len && is_word_boundary(buffer[pos])) {
        pos++;
    }

    return pos;
}

// Helper: Delete the next word
#ifdef TEST_BUILD
int delete_next_word(char *buffer, int *cursor_pos, int *buffer_len) {
#else
static int delete_next_word(char *buffer, int *cursor_pos, int *buffer_len) {
#endif
    if (*cursor_pos >= *buffer_len) return 0;

    int start_pos = *cursor_pos;
    int end_pos = move_forward_word(buffer, start_pos, *buffer_len);

    if (end_pos > start_pos) {
        // Delete characters from start_pos to end_pos
        memmove(&buffer[start_pos], &buffer[end_pos], *buffer_len - end_pos + 1);
        *buffer_len -= (end_pos - start_pos);
        return end_pos - start_pos; // Return number of characters deleted
    }

    return 0;
}

// Helper: Calculate visible length of string (excluding ANSI escape sequences)
#ifdef TEST_BUILD
int visible_strlen(const char *str) {
#else
static int visible_strlen(const char *str) {
#endif
    int visible_len = 0;
    int in_escape = 0;

    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\033') {
            // Start of ANSI escape sequence
            in_escape = 1;
        } else if (in_escape) {
            // Check if this ends the escape sequence
            if ((str[i] >= 'A' && str[i] <= 'Z') || (str[i] >= 'a' && str[i] <= 'z')) {
                in_escape = 0;
            }
        } else {
            // Regular visible character
            visible_len++;
        }
    }

    return visible_len;
}

// Helper: Redraw the input line with cursor at correct position
// Handles multiline input by displaying newlines as actual line breaks
static void redraw_input_line(const char *prompt, const char *buffer, int cursor_pos) {
    static int previous_cursor_line = 0;  // Which line (0-indexed) the cursor was on

    int buffer_len = strlen(buffer);
    int prompt_len = visible_strlen(prompt);

    // Move cursor up to start of previous input
    // Cursor was left on previous_cursor_line, so move up by that amount
    if (previous_cursor_line > 0) {
        printf("\033[%dA", previous_cursor_line);
    }

    // Clear from here down and move to start of line
    printf("\r\033[J");

    // Print prompt and entire buffer once
    printf("%s", prompt);
    for (int i = 0; i < buffer_len; i++) {
        putchar(buffer[i]);  // Prints \n naturally
    }

    // Calculate cursor position in the buffer (column within current line)
    // Also track which line (0-indexed) the cursor is on
    int col_position = 0;
    int cursor_on_first_line = 1;
    int cursor_line = 0;  // 0 = first line, 1 = second line, etc.
    for (int i = 0; i < cursor_pos; i++) {
        if (buffer[i] == '\n') {
            col_position = 0;
            cursor_on_first_line = 0;  // We've seen a newline, so not on first line
            cursor_line++;
        } else {
            col_position++;
        }
    }

    // Calculate lines after cursor
    int lines_after_cursor = 0;
    for (int i = cursor_pos; i < buffer_len; i++) {
        if (buffer[i] == '\n') lines_after_cursor++;
    }

    // Reposition cursor: move up to cursor's line, then position horizontally
    if (lines_after_cursor > 0) {
        printf("\033[%dA", lines_after_cursor);
    }
    printf("\r");  // Start of line

    // Move right to cursor position
    // Only add prompt length if cursor is on the first line
    int target_col = (cursor_on_first_line ? prompt_len : 0) + col_position;
    if (target_col > 0) {
        printf("\033[%dC", target_col);
    }

    fflush(stdout);

    // Store cursor line for next redraw
    previous_cursor_line = cursor_line;
}

// Advanced input handler with readline-like keybindings
// Returns: 1 on success, 0 on EOF, -1 on error
static int read_line_advanced(const char *prompt, char *buffer, size_t buffer_size) {
    struct termios old_term, new_term;

    // Save terminal settings
    if (tcgetattr(STDIN_FILENO, &old_term) < 0) {
        // Fall back to fgets if we can't set raw mode
        printf("%s", prompt);
        fflush(stdout);
        if (fgets(buffer, buffer_size, stdin) == NULL) {
            return 0;  // EOF
        }
        buffer[strcspn(buffer, "\n")] = 0;
        return 1;
    }

    // Set up raw mode
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    new_term.c_cc[VMIN] = 1;
    new_term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

    // Initialize buffer
    memset(buffer, 0, buffer_size);
    int len = 0;
    int cursor_pos = 0;

    // Print initial prompt
    printf("%s", prompt);
    fflush(stdout);

    int running = 1;
    while (running) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) {
            // Error or EOF
            tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
            return 0;
        }

        if (c == 27) {  // ESC sequence
            unsigned char seq[2];

            // Read next two bytes
            if (read(STDIN_FILENO, &seq[0], 1) != 1) {
                continue;
            }

            if (seq[0] == 'b' || seq[0] == 'B') {
                // Alt+b: backward word
                cursor_pos = move_backward_word(buffer, cursor_pos);
                redraw_input_line(prompt, buffer, cursor_pos);
            } else if (seq[0] == 'f' || seq[0] == 'F') {
                // Alt+f: forward word
                cursor_pos = move_forward_word(buffer, cursor_pos, len);
                redraw_input_line(prompt, buffer, cursor_pos);
            } else if (seq[0] == 'd' || seq[0] == 'D') {
                // Alt+d: delete next word
                if (delete_next_word(buffer, &cursor_pos, &len)) {
                    redraw_input_line(prompt, buffer, cursor_pos);
                }
            } else if (seq[0] == '[') {
                // Arrow keys and other escape sequences
                if (read(STDIN_FILENO, &seq[1], 1) != 1) {
                    continue;
                }

                if (seq[1] == 'D') {
                    // Left arrow
                    if (cursor_pos > 0) {
                        cursor_pos--;
                        printf("\033[D");  // Move cursor left
                        fflush(stdout);
                    }
                } else if (seq[1] == 'C') {
                    // Right arrow
                    if (cursor_pos < len) {
                        cursor_pos++;
                        printf("\033[C");  // Move cursor right
                        fflush(stdout);
                    }
                } else if (seq[1] == 'H') {
                    // Home
                    cursor_pos = 0;
                    redraw_input_line(prompt, buffer, cursor_pos);
                } else if (seq[1] == 'F') {
                    // End
                    cursor_pos = len;
                    redraw_input_line(prompt, buffer, cursor_pos);
                }
            }
        } else if (c == 1) {
            // Ctrl+A: beginning of line
            cursor_pos = 0;
            redraw_input_line(prompt, buffer, cursor_pos);
        } else if (c == 5) {
            // Ctrl+E: end of line
            cursor_pos = len;
            redraw_input_line(prompt, buffer, cursor_pos);
        } else if (c == 4) {
            // Ctrl+D: EOF only (always exits, even if buffer has content)
            printf("\n");
            tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
            return 0;
        } else if (c == 11) {
            // Ctrl+K: kill to end of line
            buffer[cursor_pos] = 0;
            len = cursor_pos;
            redraw_input_line(prompt, buffer, cursor_pos);
        } else if (c == 21) {
            // Ctrl+U: kill to beginning of line
            if (cursor_pos > 0) {
                memmove(buffer, &buffer[cursor_pos], len - cursor_pos + 1);
                len -= cursor_pos;
                cursor_pos = 0;
                redraw_input_line(prompt, buffer, cursor_pos);
            }
        } else if (c == 127 || c == 8) {
            // Backspace
            if (cursor_pos > 0) {
                memmove(&buffer[cursor_pos - 1], &buffer[cursor_pos], len - cursor_pos + 1);
                len--;
                cursor_pos--;
                redraw_input_line(prompt, buffer, cursor_pos);
            }
        } else if (c == 14) {
            // Ctrl+N (ASCII 14): Insert newline character
            if (len < (int)buffer_size - 1) {
                memmove(&buffer[cursor_pos + 1], &buffer[cursor_pos], len - cursor_pos + 1);
                buffer[cursor_pos] = '\n';
                len++;
                cursor_pos++;
                redraw_input_line(prompt, buffer, cursor_pos);
            }
        } else if (c == '\r' || c == '\n') {
            // Enter: Submit (handles both \r and \n)
            printf("\n");
            running = 0;
        } else if (c >= 32 && c < 127) {
            // Printable character
            if (len < (int)buffer_size - 1) {
                // Insert character at cursor position
                memmove(&buffer[cursor_pos + 1], &buffer[cursor_pos], len - cursor_pos + 1);
                buffer[cursor_pos] = c;
                len++;
                cursor_pos++;
                redraw_input_line(prompt, buffer, cursor_pos);
            }
        }
    }

    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    return 1;
}

static void interactive_mode(ConversationState *state) {
    // Print intro with blue mascot featuring 'C'
    printf("\n");
    printf("             %sclaude-c%s v%s\n", ANSI_CYAN, ANSI_RESET, VERSION);
    printf(" %s ▐▛███▜▌%s    Model: %s\n", ANSI_BLUE, ANSI_RESET, state->model);
    printf("%s▝▜███████▛▘%s  Directory: %s\n", ANSI_BLUE, ANSI_RESET, state->working_dir);
    printf("%s  ▘▘   ▝▝%s\n", ANSI_BLUE, ANSI_RESET);
    printf("             Commands: /exit /quit /clear /help\n");
    printf("             Keybindings: Alt+b/f/d (word), Ctrl+a/e (line), Ctrl+n (newline)\n");
    printf("             Type Ctrl+D to exit\n\n");

    char input_buffer[BUFFER_SIZE];
    int running = 1;

    while (running) {
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "%s> %s", ANSI_GREEN, ANSI_RESET);

        int result = read_line_advanced(prompt, input_buffer, sizeof(input_buffer));
        if (result == 0) {
            // EOF (Ctrl+D)
            printf("Goodbye!\n");
            break;
        } else if (result < 0) {
            // Error
            fprintf(stderr, "Error reading input\n");
            break;
        }

        // Skip empty input
        if (strlen(input_buffer) == 0) {
            continue;
        }

        // Handle commands
        if (strcmp(input_buffer, "/exit") == 0 || strcmp(input_buffer, "/quit") == 0) {
            break;
        }

        if (strcmp(input_buffer, "/clear") == 0) {
            clear_conversation(state);
            print_status("Conversation cleared");
            printf("\n");
            continue;
        }

        if (strcmp(input_buffer, "/help") == 0) {
            printf("\n%sCommands:%s\n", ANSI_CYAN, ANSI_RESET);
            printf("  /exit /quit - Exit interactive mode\n");
            printf("  /clear      - Clear conversation history\n");
            printf("  /help       - Show this help\n");
            printf("  Ctrl+D      - Exit\n\n");
            continue;
        }

        // Display user message
        print_user(input_buffer);
        printf("\n");

        // Add to conversation
        add_user_message(state, input_buffer);

        // Call API with spinner
        Spinner *api_spinner = spinner_start("Waiting for API response...", SPINNER_CYAN);
        cJSON *response = call_api(state);
        spinner_stop(api_spinner, NULL, 1);

        if (!response) {
            print_error("Failed to get response from API");
            printf("\n");
            continue;
        }

        // Check for errors
        cJSON *error = cJSON_GetObjectItem(response, "error");
        if (error) {
            cJSON *error_message = cJSON_GetObjectItem(error, "message");
            const char *error_msg = error_message ? error_message->valuestring : "Unknown error";
            print_error(error_msg);
            cJSON_Delete(response);
            printf("\n");
            continue;
        }

        process_response(state, response);
        cJSON_Delete(response);
        printf("\n");
    }

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
        fprintf(stderr, "Error: Unexpected arguments\n");
        fprintf(stderr, "Usage: %s [-h|--help]\n", argv[0]);
        return 1;
    }

    // Check for API key
    const char *api_key = getenv("OPENAI_API_KEY");
    if (!api_key) {
        fprintf(stderr, "Error: OPENAI_API_KEY environment variable not set\n");
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
        fprintf(stderr, "Warning: Failed to initialize logging system\n");
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
        fprintf(stderr, "Warning: Failed to generate session ID\n");
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

    if (!state.working_dir) {
        fprintf(stderr, "Failed to get current working directory\n");
        free(state.api_key);
        free(state.api_url);
        free(state.model);
        curl_global_cleanup();
        return 1;
    }

    // Build and add system prompt with environment context
    char *system_prompt = build_system_prompt(state.working_dir);
    if (system_prompt) {
        add_system_message(&state, system_prompt);
        free(system_prompt);
        LOG_DEBUG("System prompt added with environment context");
    } else {
        LOG_WARN("Failed to build system prompt");
    }

    // Run interactive mode
    interactive_mode(&state);

    // Cleanup
    for (int i = 0; i < state.count; i++) {
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
