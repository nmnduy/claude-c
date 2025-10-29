/*
 * commands.c - Command Registration and Dispatch Implementation
 */

#include "commands.h"
#include "claude_internal.h"
#include "logger.h"
#include "fallback_colors.h"
#define COLORSCHEME_EXTERN
#include "colorscheme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <glob.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cjson/cJSON.h>

// ============================================================================
// Command Registry
// ============================================================================

#define MAX_COMMANDS 32
static const Command *command_registry[MAX_COMMANDS];
static int command_count = 0;

// ============================================================================
// Helper Functions
// ============================================================================

static void print_status(const char *text) {
    char color_buf[32];
    const char *status_color;
    if (get_colorscheme_color(COLORSCHEME_STATUS, color_buf, sizeof(color_buf)) == 0) {
        status_color = color_buf;
    } else {
        LOG_WARN("Using fallback ANSI color for STATUS (commands)");
        status_color = ANSI_FALLBACK_STATUS;
    }
    printf("%s[Status]%s %s\n", status_color, ANSI_RESET, text);
    fflush(stdout);
}

static void print_error(const char *text) {
    char color_buf[32];
    const char *error_color;
    if (get_colorscheme_color(COLORSCHEME_ERROR, color_buf, sizeof(color_buf)) == 0) {
        error_color = color_buf;
    } else {
        LOG_WARN("Using fallback ANSI color for ERROR (commands)");
        error_color = ANSI_FALLBACK_ERROR;
    }
    fprintf(stderr, "%s[Error]%s %s\n", error_color, ANSI_RESET, text);
    fflush(stderr);
}

// ============================================================================
// Forward Declarations for Completion Functions
// ============================================================================

static CompletionResult* dir_path_completer(const char *line, int cursor_pos, void *ctx);

// ============================================================================
// Command Handlers
// ============================================================================

static int cmd_exit(ConversationState *state, const char *args) {
    (void)state; (void)args;
    return -2;  // Special code to exit
}

static int cmd_quit(ConversationState *state, const char *args) {
    return cmd_exit(state, args);
}

static int cmd_clear(ConversationState *state, const char *args) {
    (void)args;
    clear_conversation(state);
    print_status("Conversation cleared");
    printf("\n");
    return 0;
}

static int cmd_add_dir(ConversationState *state, const char *args) {
    // Trim leading whitespace from args
    while (*args == ' ' || *args == '\t') args++;
    if (strlen(args) == 0) {
        print_error("Usage: /add-dir <directory-path>");
        printf("\n");
        return -1;
    }
    if (add_directory(state, args) == 0) {
        print_status("Added directory to context");
        printf("\n");
        return 0;
    } else {
        char err_msg[PATH_MAX + 64];
        snprintf(err_msg, sizeof(err_msg),
                 "Failed to add directory: %s (not found or already added)", args);
        print_error(err_msg);
        printf("\n");
        return -1;
    }
}

static int cmd_help(ConversationState *state, const char *args) {
    (void)state; (void)args;
    char color_buf[32];
    const char *status_color;
    if (get_colorscheme_color(COLORSCHEME_STATUS, color_buf, sizeof(color_buf)) == 0) {
        status_color = color_buf;
    } else {
        LOG_WARN("Using fallback ANSI color for STATUS (help command)");
        status_color = ANSI_FALLBACK_STATUS;
    }
    printf("\n%sCommands:%s\n", status_color, ANSI_RESET);
    for (int i = 0; i < command_count; i++) {
        const Command *cmd = command_registry[i];
        printf("  %-18s - %s\n", cmd->usage, cmd->description);
    }
    printf("  Ctrl+D             - Exit\n\n");
    return 0;
}

// ============================================================================
// Command Definitions
// ============================================================================

static Command exit_cmd = {
    .name = "exit",
    .usage = "/exit",
    .description = "Exit interactive mode",
    .handler = cmd_exit,
    .completer = commands_tab_completer
};

static Command quit_cmd = {
    .name = "quit",
    .usage = "/quit",
    .description = "Exit interactive mode",
    .handler = cmd_quit,
    .completer = commands_tab_completer
};

static Command clear_cmd = {
    .name = "clear",
    .usage = "/clear",
    .description = "Clear conversation history",
    .handler = cmd_clear,
    .completer = commands_tab_completer
};

static Command add_dir_cmd = {
    .name = "add-dir",
    .usage = "/add-dir <path>",
    .description = "Add directory to working directories",
    .handler = cmd_add_dir,
    .completer = dir_path_completer
};

static Command help_cmd = {
    .name = "help",
    .usage = "/help",
    .description = "Show this help",
    .handler = cmd_help,
    .completer = commands_tab_completer
};

// ============================================================================
// API Implementation
// ============================================================================

void commands_init(void) {
    command_count = 0;
    commands_register(&exit_cmd);
    commands_register(&quit_cmd);
    commands_register(&clear_cmd);
    commands_register(&add_dir_cmd);
    commands_register(&help_cmd);
}

void commands_register(const Command *cmd) {
    if (command_count < MAX_COMMANDS) {
        command_registry[command_count++] = cmd;
    } else {
        LOG_WARN("Command registry full, cannot register '%s'", cmd->name);
    }
}

int commands_execute(ConversationState *state, const char *input) {
    if (!input || input[0] != '/') return -1;
    const char *cmd_line = input + 1;
    const char *space = strchr(cmd_line, ' ');
    size_t cmd_len = space ? (size_t)(space - cmd_line) : strlen(cmd_line);
    const char *args = space ? space + 1 : "";
    for (int i = 0; i < command_count; i++) {
        const Command *cmd = command_registry[i];
        if (strlen(cmd->name) == cmd_len && strncmp(cmd->name, cmd_line, cmd_len) == 0) {
            return cmd->handler(state, args);
        }
    }
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Unknown command: %.*s", (int)cmd_len, cmd_line);
    print_error(err_msg);
    printf("Type /help for available commands\n\n");
    return -1;
}

const Command** commands_list(int *count) {
    *count = command_count;
    return command_registry;
}

// ============================================================================
// Tab Completion Implementations
// ============================================================================

CompletionResult* commands_tab_completer(const char *line, int cursor_pos, void *ctx) {
    (void)ctx;  // Suppress unused parameter warning
    if (!line || line[0] != '/') return NULL;
    const char *space = strchr(line, ' ');
    int cmd_name_len = space ? (int)(space - line - 1) : ((int)strlen(line) - 1);
    int name_end_pos = cmd_name_len + 1;
    if (cursor_pos <= name_end_pos) {
        // Complete command names
        int match_count = 0;
        for (int i = 0; i < command_count; i++) {
            if (strncmp(command_registry[i]->name, line + 1, (size_t)cmd_name_len) == 0) match_count++;
        }
        if (match_count == 0) return NULL;
        CompletionResult *res = malloc(sizeof(CompletionResult));
        res->options = malloc(sizeof(char*) * (size_t)match_count);
        res->count = 0; res->selected = 0;
        for (int i = 0; i < command_count; i++) {
            const char *name = command_registry[i]->name;
            if (strncmp(name, line + 1, (size_t)cmd_name_len) == 0) {
                char *opt = malloc(strlen(name) + 2);
                snprintf(opt, strlen(name) + 2, "/%s", name);
                res->options[res->count++] = opt;
            }
        }
        return res;
    } else {
        // Delegate argument completion
        // Identify command name
        char cmd_name[64];
        int clen = cmd_name_len;
        if (clen >= (int)sizeof(cmd_name)) clen = sizeof(cmd_name) - 1;
        memcpy(cmd_name, line + 1, clen);
        cmd_name[clen] = '\0';
        for (int i = 0; i < command_count; i++) {
            const Command *cmd = command_registry[i];
            if (strcmp(cmd->name, cmd_name) == 0 && cmd->completer) {
                return cmd->completer(line, cursor_pos, ctx);
            }
        }
        return NULL;
    }
}

static CompletionResult* dir_path_completer(const char *line, int cursor_pos, void *ctx) {
    (void)ctx;
    const char *arg = strchr(line, ' ');
    if (!arg) return NULL;
    arg++;
    int arg_start = (int)(arg - line);
    int arg_len = cursor_pos - arg_start;
    if (arg_len < 0) arg_len = 0;
    char prefix[PATH_MAX];
    int plen = arg_len < PATH_MAX ? arg_len : PATH_MAX - 1;
    memcpy(prefix, arg, plen);
    prefix[plen] = '\0';
    char pattern[PATH_MAX];
    if (plen == 0) strcpy(pattern, "*"); else snprintf(pattern, sizeof(pattern), "%s*", prefix);
    glob_t globbuf;
    int ret = glob(pattern, GLOB_MARK | GLOB_NOSORT, NULL, &globbuf);
    if (ret != 0) { globfree(&globbuf); return NULL; }
    size_t dir_count = 0;
    for (size_t i = 0; i < globbuf.gl_pathc; i++) {
        const char *m = globbuf.gl_pathv[i];
        size_t len = strlen(m);
        if (len > 0 && m[len-1] == '/') dir_count++;
    }
    if (dir_count == 0) { globfree(&globbuf); return NULL; }
    CompletionResult *res = malloc(sizeof(CompletionResult));
    res->options = malloc(sizeof(char*) * dir_count);
    res->count = 0; res->selected = 0;
    for (size_t i = 0; i < globbuf.gl_pathc; i++) {
        const char *m = globbuf.gl_pathv[i];
        size_t len = strlen(m);
        if (len > 0 && m[len-1] == '/') {
            res->options[res->count++] = strdup(m);
        }
    }
    globfree(&globbuf);
    return res;
}
