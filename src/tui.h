/*
 * TUI (Terminal User Interface) - ncurses-based interface for Claude Code
 *
 * Provides a full-screen TUI with:
 * - Scrollable conversation window (top)
 * - Status line (middle)
 * - Input area (bottom)
 */

#ifndef TUI_H
#define TUI_H

#include <ncurses.h>
#include "claude_internal.h"

// TUI Color pairs
typedef enum {
    COLOR_PAIR_DEFAULT = 1,
    COLOR_PAIR_USER = 2,       // Green for user messages
    COLOR_PAIR_ASSISTANT = 3,  // Blue for assistant messages
    COLOR_PAIR_TOOL = 4,       // Yellow for tool execution
    COLOR_PAIR_ERROR = 5,      // Red for errors
    COLOR_PAIR_STATUS = 6,     // Cyan for status messages
    COLOR_PAIR_PROMPT = 7      // Green for input prompt
} TUIColorPair;

// TUI State
typedef struct {
    WINDOW *conv_win;        // Conversation window (scrollable)
    WINDOW *status_win;      // Status line window
    WINDOW *input_win;       // Input window

    int screen_height;       // Terminal height
    int screen_width;        // Terminal width

    int conv_height;         // Height of conversation window
    int input_height;        // Height of input window (fixed at 4 lines)

    char **conv_lines;       // Array of conversation lines
    int conv_lines_count;    // Number of lines in conversation
    int conv_lines_capacity; // Allocated capacity
    int conv_scroll_offset;  // Current scroll position (0 = bottom)

    int is_initialized;      // Whether TUI has been set up
} TUIState;

// Initialize the TUI
// Returns: 0 on success, -1 on failure
int tui_init(TUIState *tui);

// Cleanup and restore terminal
void tui_cleanup(TUIState *tui);

// Add a line to the conversation window
// prefix: Color prefix (e.g., "[User]", "[Assistant]")
// text: Message text
// color_pair: Color pair to use for the message
void tui_add_conversation_line(TUIState *tui, const char *prefix, const char *text, TUIColorPair color_pair);

// Update the status line
void tui_update_status(TUIState *tui, const char *status_text);

// Read input from the user
// Returns: Newly allocated string with user input, or NULL on EOF
// Caller must free the returned string
char* tui_read_input(TUIState *tui, const char *prompt);

// Refresh all windows
void tui_refresh(TUIState *tui);

// Clear the conversation window
void tui_clear_conversation(TUIState *tui);

// Handle window resize
void tui_handle_resize(TUIState *tui);

#endif // TUI_H
