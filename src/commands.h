/*
 * commands.h - Command Registration and Dispatch System
 *
 * Provides a table-driven command system for slash commands like:
 * /exit, /quit, /clear, /add-dir, /help
 */

#ifndef COMMANDS_H
#define COMMANDS_H

#include "ncurses_input.h"  // For CompletionResult and CompletionFn types
#include "claude_internal.h"

// ============================================================================
// Command Definition
// ============================================================================

typedef struct {
    const char *name;         // Command name (without '/' prefix), e.g., "add-dir"
    const char *usage;        // Usage string, e.g., "/add-dir <path>"
    const char *description;  // One-line description for /help
    int (*handler)(ConversationState *state, const char *args);  // Handler function
    CompletionFn completer;   // Optional: tab completion for arguments
} Command;

// ============================================================================
// API Functions
// ============================================================================

/**
 * Initialize the command system
 * Registers all built-in commands
 */
void commands_init(void);

/**
 * Register a new command
 *
 * @param cmd  Pointer to Command struct (must remain valid)
 */
void commands_register(const Command *cmd);

/**
 * Execute a command from user input
 *
 * @param state  Conversation state
 * @param input  Full input line (including '/' prefix)
 * @return       0 on success, -1 if command not found, -2 to exit
 */
int commands_execute(ConversationState *state, const char *input);

/**
 * Get list of all registered commands
 *
 * @param count  Pointer to int that will receive command count
 * @return       Array of Command pointers
 */
const Command** commands_list(int *count);


/**
 * Tab completion dispatcher for commands
 *
 * @param line        Full input line
 * @param cursor_pos  Cursor position in line
 * @param ctx         ConversationState pointer
 * @return            CompletionResult* or NULL
 */
CompletionResult* commands_tab_completer(const char *line, int cursor_pos, void *ctx);

#endif // COMMANDS_H
