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

#include "claude_internal.h"
#include "todo.h"

// Forward declaration for WINDOW type (not actually used, kept for compatibility)
typedef struct _win_st WINDOW;

typedef struct TUIInputBuffer TUIInputBuffer;

// TUI Color pairs
typedef enum {
    COLOR_PAIR_DEFAULT = 1,    // Foreground color for main text
    COLOR_PAIR_FOREGROUND = 2, // Explicit foreground color
    COLOR_PAIR_USER = 3,       // Green for user role names
    COLOR_PAIR_ASSISTANT = 4,  // Blue for assistant role names
    COLOR_PAIR_TOOL = 5,       // Yellow for tool execution indicators
    COLOR_PAIR_ERROR = 6,      // Red for errors
    COLOR_PAIR_STATUS = 7,     // Cyan for status messages
    COLOR_PAIR_PROMPT = 8      // Green for input prompt
} TUIColorPair;

// Conversation message entry
typedef struct {
    char *prefix;            // Role prefix (e.g., "[User]", "[Assistant]")
    char *text;              // Message text
    TUIColorPair color_pair; // Color for display
} ConversationEntry;

// TUI State
typedef struct {
    WINDOW *conv_win;        // Conversation window (top of screen)
    WINDOW *status_win;      // Status window (single-line separator)
    WINDOW *input_win;       // Input window at bottom
    TUIInputBuffer *input_buffer; // Persistent input buffer state

    int screen_height;       // Terminal height
    int screen_width;        // Terminal width

    int conv_height;         // Height of conversation window
    int input_height;        // Height of input window (dynamic 3-5 lines)
    int status_height;       // Height of status window (currently 1)

    ConversationEntry *entries;    // Array of conversation entries
    int entries_count;             // Number of entries
    int entries_capacity;          // Capacity of entries array
    int conv_scroll_offset;        // Scroll offset (lines from top)

    char *status_message;    // Current status text (owned by TUI)
    int status_visible;      // Whether status should be shown

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

// Refresh all windows
void tui_refresh(TUIState *tui);

// Clear the conversation window
void tui_clear_conversation(TUIState *tui);

// Handle window resize
void tui_handle_resize(TUIState *tui);

// Check if window resize is pending
// Returns: Non-zero if resize signal was received, 0 otherwise
int tui_resize_pending(void);

// Clear the resize pending flag
void tui_clear_resize_flag(void);

// Display startup banner with blue mascot
// version: Version string (e.g., "0.0.1")
// model: Model name
// working_dir: Current working directory
void tui_show_startup_banner(TUIState *tui, const char *version, const char *model, const char *working_dir);

// Render TODO list panel
// Shows task list with visual indicators (✓ completed, ⋯ in progress, ○ pending)
void tui_render_todo_list(TUIState *tui, const TodoList *todo_list);

// Scroll conversation window
// direction: positive to scroll down, negative to scroll up
void tui_scroll_conversation(TUIState *tui, int direction);

// ============================================================================
// Phase 2: Non-blocking Input and Event Loop
// ============================================================================

// Poll for keyboard input (non-blocking)
// Returns: Character code, ERR if no input available, or special key codes
// This is the non-blocking version of wgetch()
int tui_poll_input(TUIState *tui);

// Process a single input character
// Returns: 0 if handled, 1 if Enter pressed (submit), -1 on EOF/quit
int tui_process_input_char(TUIState *tui, int ch, const char *prompt);

// Get the current input buffer content
// Returns: Pointer to internal buffer (do NOT free), or NULL if empty
const char* tui_get_input_buffer(TUIState *tui);

// Clear the input buffer
void tui_clear_input_buffer(TUIState *tui);

// Redraw the input window with current buffer state
void tui_redraw_input(TUIState *tui, const char *prompt);

// Event loop callback for processing user input submissions
// Called when user presses Enter with non-empty input
// Returns: 0 to continue, non-zero to exit event loop
typedef int (*InputSubmitCallback)(const char *input, void *user_data);

// Main event loop (non-blocking, ~60 FPS)
// Processes input, handles resize, and processes TUI message queue
// prompt: Input prompt to display
// callback: Function to call when user submits input
// user_data: Opaque pointer passed to callback
// msg_queue: Optional TUI message queue to process (can be NULL)
// Returns: 0 on normal exit, -1 on error
int tui_event_loop(TUIState *tui, const char *prompt, 
                   InputSubmitCallback callback, void *user_data,
                   void *msg_queue);

#endif // TUI_H
