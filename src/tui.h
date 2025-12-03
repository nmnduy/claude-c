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

#include <stdint.h>
#include "claude_internal.h"
#include "todo.h"
#include "window_manager.h"
#include "history_file.h"
#ifndef TEST_BUILD
#include "persistence.h"
#else
// Forward declaration for test builds
struct PersistenceDB;
#endif

// Forward declaration for WINDOW type (not actually used, kept for compatibility)
typedef struct _win_st WINDOW;

typedef struct TUIInputBuffer TUIInputBuffer;

// TUI Color pairs
typedef enum {
    COLOR_PAIR_DEFAULT = 1,    // Foreground color for main text
    COLOR_PAIR_FOREGROUND = 2, // Explicit foreground color
    COLOR_PAIR_USER = 3,       // Green for user role names
    COLOR_PAIR_ASSISTANT = 4,  // Blue for assistant role names
    COLOR_PAIR_TOOL = 5,       // Cyan for tool execution indicators (softer)
    COLOR_PAIR_ERROR = 6,      // Red for errors
    COLOR_PAIR_STATUS = 7,     // Cyan for status messages
    COLOR_PAIR_PROMPT = 8,     // Green for input prompt
    COLOR_PAIR_TODO_COMPLETED = 9,   // Green for completed tasks
    COLOR_PAIR_TODO_IN_PROGRESS = 10, // Yellow for in-progress tasks
    COLOR_PAIR_TODO_PENDING = 11     // Cyan/Blue for pending tasks
} TUIColorPair;

// Conversation message entry
typedef struct {
    char *prefix;            // Role prefix (e.g., "[User]", "[Assistant]")
    char *text;              // Message text
    TUIColorPair color_pair; // Color for display
} ConversationEntry;

// TUI Mode (Vim-like)
typedef enum {
    TUI_MODE_NORMAL,   // Normal mode (vim-like navigation, default for conversation viewing)
    TUI_MODE_INSERT,   // Insert mode (text input for sending messages)
    TUI_MODE_COMMAND   // Command mode (entered with ':' from normal mode)
} TUIMode;

// TUI State
typedef struct {
    // Centralized window manager (owns ncurses windows)
    WindowManager wm;

    // Input buffer state
    TUIInputBuffer *input_buffer;

    // Conversation entries (source of truth used to rebuild pad on resize)
    ConversationEntry *entries;
    int entries_count;
    int entries_capacity;

    // Status state
    char *status_message;    // Current status text (owned by TUI)
    int status_visible;      // Whether status should be shown
    int status_spinner_active;        // Spinner animation active flag
    int status_spinner_frame;         // Current spinner frame index
    uint64_t status_spinner_last_update_ns; // Last spinner frame update timestamp



    // Database connection for real-time token usage queries
    struct PersistenceDB *persistence_db;  // Database connection for token queries
    char *session_id;                     // Current session ID for token queries

    // Reference to conversation state (source of truth for plan_mode and other state)
    ConversationState *conversation_state;

    // Modes
    TUIMode mode;            // Current input mode (NORMAL, INSERT, or COMMAND)
    int normal_mode_last_key; // Previous key in normal mode (for gg, G combos)
    char *command_buffer;    // Buffer for command mode input (starts with ':')
    int command_buffer_len;  // Length of command buffer
    int command_buffer_capacity; // Capacity of command buffer

    int is_initialized;      // Whether TUI has been set up

    // Persistent input history (memory + DB)
    char **input_history;    // Array of history strings (oldest -> newest)
    int input_history_count; // Number of entries loaded
    int input_history_capacity; // Capacity of array
    int input_history_pos;   // Navigation position (-1 when not navigating)
    char *input_saved_before_history; // Saved buffer when starting nav
    HistoryFile *history_file;   // Open flat file handle (kept open)
} TUIState;

// Initialize the TUI
// tui: TUI state to initialize
// state: Conversation state (source of truth for plan_mode and other state)
// Returns: 0 on success, -1 on failure
int tui_init(TUIState *tui, ConversationState *state);

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
int tui_process_input_char(TUIState *tui, int ch, const char *prompt, void *user_data);

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

// Event loop callback for handling interrupt requests
// Called when user presses Ctrl+C in INSERT mode
// Returns: 0 to continue, non-zero to exit event loop
typedef int (*InterruptCallback)(void *user_data);

// Event loop callback for handling any keypress
// Called when user types any character (before processing)
// Useful for resetting state on user activity
// Returns: void (does not affect event loop flow)
typedef void (*KeypressCallback)(void *user_data);

// Main event loop (non-blocking, ~60 FPS)
// Processes input, handles resize, and processes TUI message queue
// prompt: Input prompt to display
// submit_callback: Function to call when user submits input
// interrupt_callback: Function to call when user presses Ctrl+C (can be NULL)
// keypress_callback: Function to call on any keypress (can be NULL)
// user_data: Opaque pointer passed to callbacks
// msg_queue: Optional TUI message queue to process (can be NULL)
// Returns: 0 on normal exit, -1 on error
int tui_event_loop(TUIState *tui, const char *prompt,
                   InputSubmitCallback submit_callback,
                   InterruptCallback interrupt_callback,
                   KeypressCallback keypress_callback,
                   void *user_data,
                   void *msg_queue);

// Drain any remaining messages after the event loop stops
void tui_drain_message_queue(TUIState *tui, const char *prompt, void *msg_queue);

// Render a TODO list with colored items based on status
// list: TodoList to render
// Each item will be rendered with its status-specific color
void tui_render_todo_list(TUIState *tui, const TodoList *list);

// Update token usage counts displayed in status bar
// prompt_tokens: Total input tokens used




#endif // TUI_H
