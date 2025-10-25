/*
 * commands.c - Command Registration and Dispatch Implementation
 */

#include "commands.h"
#include "claude_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>
#include <cjson/cJSON.h>

// ANSI color codes for output
#define ANSI_RESET "\033[0m"
#define ANSI_CYAN "\033[36m"
#define ANSI_GREEN "\033[32m"
#define ANSI_RED "\033[31m"

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
    printf("%s[Status]%s %s\n", ANSI_CYAN, ANSI_RESET, text);
    fflush(stdout);
}

static void print_error(const char *text) {
    fprintf(stderr, "%s[Error]%s %s\n", ANSI_RED, ANSI_RESET, text);
    fflush(stderr);
}

// ============================================================================
// Command Handlers
// ============================================================================

static int cmd_exit(ConversationState *state, const char *args) {
    (void)state;  // Unused
    (void)args;   // Unused
    return -2;  // Special code to exit
}

static int cmd_quit(ConversationState *state, const char *args) {
    return cmd_exit(state, args);  // Same as exit
}

static int cmd_clear(ConversationState *state, const char *args) {
    (void)args;  // Unused
    clear_conversation(state);
    print_status("Conversation cleared");
    printf("\n");
    return 0;
}

static int cmd_add_dir(ConversationState *state, const char *args) {
    // Trim leading whitespace from args
    while (*args == ' ' || *args == '\t') {
        args++;
    }

    if (strlen(args) == 0) {
        print_error("Usage: /add-dir <directory-path>");
        printf("\n");
        return -1;
    }

    if (add_directory(state, args) == 0) {
        print_status("Added directory to context");

        // Rebuild and update system message with new context
        char *new_system_prompt = build_system_prompt(state);
        if (new_system_prompt) {
            // Access state->messages[0] to update system message
            // This requires ConversationState definition
            // For now, we'll handle this in the caller
            free(new_system_prompt);
        }
        return 0;
    } else {
        char err_msg[PATH_MAX + 64];
        snprintf(err_msg, sizeof(err_msg),
                 "Failed to add directory: %s (not found, not a directory, or already added)",
                 args);
        print_error(err_msg);
        printf("\n");
        return -1;
    }
}

static int cmd_help(ConversationState *state, const char *args) {
    (void)state;  // Unused
    (void)args;   // Unused

    printf("\n%sCommands:%s\n", ANSI_CYAN, ANSI_RESET);

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
    .completer = NULL
};

static Command quit_cmd = {
    .name = "quit",
    .usage = "/quit",
    .description = "Exit interactive mode",
    .handler = cmd_quit,
    .completer = NULL
};

static Command clear_cmd = {
    .name = "clear",
    .usage = "/clear",
    .description = "Clear conversation history",
    .handler = cmd_clear,
    .completer = NULL
};

static Command add_dir_cmd = {
    .name = "add-dir",
    .usage = "/add-dir <path>",
    .description = "Add directory to working directories",
    .handler = cmd_add_dir,
    .completer = NULL  // TODO: Add directory path completion
};

static Command help_cmd = {
    .name = "help",
    .usage = "/help",
    .description = "Show this help",
    .handler = cmd_help,
    .completer = NULL
};

// ============================================================================
// API Implementation
// ============================================================================

void commands_init(void) {
    command_count = 0;

    // Register built-in commands
    commands_register(&exit_cmd);
    commands_register(&quit_cmd);
    commands_register(&clear_cmd);
    commands_register(&add_dir_cmd);
    commands_register(&help_cmd);
}

void commands_register(const Command *cmd) {
    if (command_count >= MAX_COMMANDS) {
        fprintf(stderr, "Warning: Command registry full, cannot register '%s'\n", cmd->name);
        return;
    }

    command_registry[command_count++] = cmd;
}

int commands_execute(ConversationState *state, const char *input) {
    // Input should start with '/'
    if (input[0] != '/') {
        return -1;  // Not a command
    }

    // Skip the '/' prefix
    const char *cmd_line = input + 1;

    // Find where command name ends (space or end of string)
    const char *space = strchr(cmd_line, ' ');
    size_t cmd_len;
    const char *args;

    if (space) {
        cmd_len = space - cmd_line;
        args = space + 1;  // Arguments start after the space
    } else {
        cmd_len = strlen(cmd_line);
        args = "";  // No arguments
    }

    // Find matching command
    for (int i = 0; i < command_count; i++) {
        const Command *cmd = command_registry[i];
        if (strncmp(cmd->name, cmd_line, cmd_len) == 0 && strlen(cmd->name) == cmd_len) {
            // Found matching command, execute it
            return cmd->handler(state, args);
        }
    }

    // Command not found
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
