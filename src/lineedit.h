/*
 * lineedit.h - Generic Line Editor with Completion Support
 *
 * Provides readline-like functionality with:
 * - Cursor movement (arrow keys, Ctrl+a/e, Alt+b/f)
 * - Text editing (insert, delete, backspace)
 * - Word operations (Alt+d, Alt+backspace)
 * - Line operations (Ctrl+k, Ctrl+u)
 * - Tab completion support (via callback)
 * - Multiline input (Ctrl+n, bracketed paste)
 */

#ifndef LINEEDIT_H
#define LINEEDIT_H

#include <stddef.h>

// Forward declaration
typedef struct LineEditor LineEditor;

// ============================================================================
// History Support
// ============================================================================

typedef struct {
    char **entries;      // Array of history strings
    int capacity;        // Max entries (default: 100)
    int count;           // Current number of entries
    int position;        // Current position when navigating (-1 = not navigating)
} History;

// ============================================================================
// Completion Support
// ============================================================================

typedef struct {
    char **options;      // Array of completion options
    int count;           // Number of options
    int selected;        // Which option is highlighted (for cycling)
} CompletionResult;

// Completion callback: given line + cursor position, return suggestions
// Context pointer can be used to pass ConversationState or other data
typedef CompletionResult* (*CompletionFn)(const char *line, int cursor_pos, void *ctx);

// ============================================================================
// Line Editor
// ============================================================================

// Input queue for ungetch-like functionality
#define INPUT_QUEUE_SIZE 16

typedef struct LineEditor {
    char *buffer;            // Input buffer (dynamically allocated)
    size_t buffer_capacity;  // Capacity of buffer
    int cursor;              // Cursor position (0 to length)
    int length;              // Current length of input
    CompletionFn completer;  // Optional: for tab completion
    void *completer_ctx;     // Context passed to completer
    History history;         // Command history
    // Input queue for ungetch
    unsigned char input_queue[INPUT_QUEUE_SIZE];
    int queue_head;          // Read position
    int queue_tail;          // Write position
    int queue_count;         // Number of bytes in queue
    // Paste content tracking
    char *paste_content;     // Actual pasted content (kept separate from visible buffer)
    size_t paste_content_len; // Length of pasted content
    int paste_placeholder_start; // Start position of placeholder in buffer
    int paste_placeholder_len;   // Length of placeholder in buffer
} LineEditor;

// ============================================================================
// API Functions
// ============================================================================

/**
 * Initialize a line editor
 *
 * @param ed         Pointer to LineEditor struct to initialize
 * @param completer  Optional completion callback (can be NULL)
 * @param ctx        Optional context passed to completer (can be NULL)
 */
void lineedit_init(LineEditor *ed, CompletionFn completer, void *ctx);

/**
 * Read a line of input with editing support
 *
 * Returns: Newly allocated string with input (caller must free)
 *          NULL on EOF (Ctrl+D)
 *
 * @param ed      Pointer to initialized LineEditor
 * @param prompt  Prompt string to display
 */
char* lineedit_readline(LineEditor *ed, const char *prompt);

/**
 * Free resources associated with line editor
 *
 * @param ed  Pointer to LineEditor to cleanup
 */
void lineedit_free(LineEditor *ed);

/**
 * Free a completion result
 *
 * @param result  Pointer to CompletionResult to free
 */
void completion_free(CompletionResult *result);

#endif // LINEEDIT_H
