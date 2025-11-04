/*
 * lineedit.c - Generic Line Editor Implementation
 *
 * Extracted from claude.c to provide reusable line editing functionality
 */

#include "lineedit.h"
#include "logger.h"
#include "paste_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <errno.h>

#define INITIAL_BUFFER_SIZE 4096
#define DEFAULT_HISTORY_SIZE 100

// Forward declarations for TEST_BUILD
#ifdef TEST_BUILD
int utf8_char_length(unsigned char first_byte);
int is_utf8_continuation(unsigned char byte);
int is_word_boundary(char c);
int move_backward_word(const char *buffer, int cursor_pos);
int move_forward_word(const char *buffer, int cursor_pos, int buffer_len);
int visible_strlen(const char *str);
void calculate_cursor_position(
    const char *buffer,
    int buffer_len,
    int cursor_pos,
    int prompt_len,
    int term_width,
    int *out_cursor_line,
    int *out_cursor_col,
    int *out_total_lines
);
#endif

// ============================================================================
// History Management
// ============================================================================

// Initialize history
static void history_init(History *hist) {
    hist->capacity = DEFAULT_HISTORY_SIZE;
    hist->entries = malloc(sizeof(char*) * (size_t)hist->capacity);
    if (!hist->entries) {
        LOG_ERROR("Failed to allocate history buffer");
        exit(1);
    }
    hist->count = 0;
    hist->position = -1;
}

// Add entry to history (avoids duplicates of last entry)
static void history_add(History *hist, const char *entry) {
    if (!entry || entry[0] == '\0') {
        return;  // Don't add empty entries
    }

    // Don't add if it's the same as the last entry
    if (hist->count > 0 && strcmp(hist->entries[hist->count - 1], entry) == 0) {
        return;
    }

    // If at capacity, remove oldest entry
    if (hist->count >= hist->capacity) {
        free(hist->entries[0]);
        memmove(&hist->entries[0], &hist->entries[1],
                sizeof(char*) * (size_t)(hist->capacity - 1));
        hist->count--;
    }

    // Add new entry
    hist->entries[hist->count] = strdup(entry);
    hist->count++;
    hist->position = -1;  // Reset navigation position
}

// Free history
static void history_free(History *hist) {
    for (int i = 0; i < hist->count; i++) {
        free(hist->entries[i]);
    }
    free(hist->entries);
    hist->entries = NULL;
    hist->count = 0;
    hist->capacity = 0;
    hist->position = -1;
}

// ============================================================================
// Terminal State Management - Ensures terminal is always restored
// ============================================================================

static struct termios g_original_termios;
static int g_terminal_modified = 0;
static int g_cleanup_registered = 0;

// Restore terminal to original state
static void restore_terminal(void) {
    if (g_terminal_modified) {
        // Disable bracketed paste mode
        printf("\033[?2004l");
        // Show cursor (restore visibility)
        printf("\033[?25h");
        fflush(stdout);

        // Restore terminal settings
        tcsetattr(STDIN_FILENO, TCSANOW, &g_original_termios);
        g_terminal_modified = 0;
    }
}

// Ctrl+C double-press tracking
static volatile sig_atomic_t g_sigint_count = 0;
static volatile time_t g_last_sigint_time = 0;
#define SIGINT_TIMEOUT_SECONDS 2

// Signal handler for Ctrl+C
static void sigint_handler(int signum) {
    (void)signum;  // Unused parameter

    time_t now = time(NULL);

    // Check if this is a second Ctrl+C within timeout
    if (g_sigint_count > 0 && (now - g_last_sigint_time) <= SIGINT_TIMEOUT_SECONDS) {
        // Second Ctrl+C detected - exit immediately
        restore_terminal();
        fprintf(stderr, "\nExiting...\n");
        _exit(130);  // Standard exit code for SIGINT
    }

    // First Ctrl+C - set flag and display message
    g_sigint_count = 1;
    g_last_sigint_time = now;

    // Display message (safe for signal handler - using write())
    const char *msg = "\n^C (Press Ctrl+C again to exit)\n";
    ssize_t ret = write(STDERR_FILENO, msg, strlen(msg));
    (void)ret;  // Ignore write result in signal handler
}

// Signal handler for other termination signals (SIGTERM, SIGHUP, SIGQUIT)
static void signal_handler(int signum) {
    restore_terminal();

    // Re-raise the signal with default handler to allow proper exit
    signal(signum, SIG_DFL);
    raise(signum);
}

// Register cleanup handlers (called once)
static void register_cleanup_handlers(void) {
    if (g_cleanup_registered) {
        return;  // Already registered
    }

    // Register atexit handler
    atexit(restore_terminal);

    // Register signal handlers for common termination signals
    signal(SIGINT, sigint_handler);   // Ctrl+C (double-press to exit)
    signal(SIGTERM, signal_handler);  // kill command
    signal(SIGHUP, signal_handler);   // Terminal hangup
    signal(SIGQUIT, signal_handler);  /* Ctrl+\ */

    g_cleanup_registered = 1;
}

// ============================================================================
// Helper Functions
// ============================================================================

// UTF-8 Helper Functions
// UTF-8 encoding:
// - 1 byte:  0xxxxxxx (0x00-0x7F, ASCII)
// - 2 bytes: 110xxxxx 10xxxxxx (0xC0-0xDF start)
// - 3 bytes: 1110xxxx 10xxxxxx 10xxxxxx (0xE0-0xEF start)
// - 4 bytes: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx (0xF0-0xF7 start)

// Get the number of bytes in a UTF-8 character from its first byte
#ifdef TEST_BUILD
int
#else
static int
#endif
utf8_char_length(unsigned char first_byte) {
    if ((first_byte & 0x80) == 0) return 1;  // 0xxxxxxx
    if ((first_byte & 0xE0) == 0xC0) return 2;  // 110xxxxx
    if ((first_byte & 0xF0) == 0xE0) return 3;  // 1110xxxx
    if ((first_byte & 0xF8) == 0xF0) return 4;  // 11110xxx
    return 1;  // Invalid, treat as single byte
}

// Check if a byte is a UTF-8 continuation byte (10xxxxxx)
#ifdef TEST_BUILD
int
#else
static int
#endif
is_utf8_continuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

// Read a complete UTF-8 character from stdin
// Returns the number of bytes read (1-4), or 0 on error
// The character is stored in the buffer
static int read_utf8_char(unsigned char *buffer, unsigned char first_byte) {
    buffer[0] = first_byte;
    int expected_length = utf8_char_length(first_byte);

    if (expected_length == 1) {
        return 1;  // ASCII, already have it
    }

    // Read continuation bytes
    for (int i = 1; i < expected_length; i++) {
        if (read(STDIN_FILENO, &buffer[i], 1) != 1) {
            return 0;  // Read error
        }

        // Verify it's a valid continuation byte
        if (!is_utf8_continuation(buffer[i])) {
            return 1;  // Invalid UTF-8, return just the first byte
        }
    }

    return expected_length;
}

// Forward declarations for buffer operations
static int buffer_delete_range(LineEditor *ed, int start, int end);

// Check if character is word boundary
#ifdef TEST_BUILD
int
#else
static int
#endif
is_word_boundary(char c) {
    return !isalnum(c) && c != '_';
}

// Move cursor backward by one word
#ifdef TEST_BUILD
int
#else
static int
#endif
move_backward_word(const char *buffer, int cursor_pos) {
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

// Move cursor forward by one word
#ifdef TEST_BUILD
int
#else
static int
#endif
move_forward_word(const char *buffer, int cursor_pos, int buffer_len) {
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

// Delete the next word (now using LineEditor)
static int delete_next_word(LineEditor *ed) {
    if (ed->cursor >= ed->length) return 0;

    int start_pos = ed->cursor;
    int end_pos = move_forward_word(ed->buffer, start_pos, ed->length);

    if (end_pos > start_pos) {
        // Delete characters from start_pos to end_pos
        return buffer_delete_range(ed, start_pos, end_pos);
    }

    return 0;
}

// Calculate visible length of string (excluding ANSI escape sequences)
#ifdef TEST_BUILD
int
#else
static int
#endif
visible_strlen(const char *str) {
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

// Get terminal width
static int get_terminal_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;  // Default fallback
}

// Calculate cursor position accounting for wrapping
// Returns cursor line and column via output parameters
// This function is extracted for testing
#ifdef TEST_BUILD
void
#else
static void
#endif
calculate_cursor_position(
    const char *buffer,
    int buffer_len,
    int cursor_pos,
    int prompt_len,
    int term_width,
    int *out_cursor_line,
    int *out_cursor_col,
    int *out_total_lines
) {
    int col = prompt_len;  // Start with prompt on first line
    int line = 0;
    int cursor_line = 0;
    int cursor_col = 0;
    int past_cursor = 0;

    for (int i = 0; i < buffer_len; i++) {
        // Check if we're at the cursor position
        if (i == cursor_pos && !past_cursor) {
            cursor_line = line;
            cursor_col = col;
            past_cursor = 1;
        }

        if (buffer[i] == '\n') {
            // Manual newline: move to next line, reset column
            line++;
            col = 0;
        } else {
            // Regular character
            col++;
            // Check for automatic wrapping at terminal edge
            // Wrap happens when we exceed the width, not when we reach it
            if (col > term_width) {
                line++;
                col = 1;  // Already printed one char on new line
            }
        }
    }

    // Handle case where cursor is at end of buffer
    if (cursor_pos >= buffer_len) {
        cursor_line = line;
        cursor_col = col;
    }

    *out_cursor_line = cursor_line;
    *out_cursor_col = cursor_col;
    *out_total_lines = line;
}

// Redraw the input line with cursor at correct position
// Handles both manual newlines and automatic terminal wrapping
// If force_reset is true, doesn't use previous position (useful after display disruption)
static void redraw_input_line_internal(const char *prompt, const char *buffer, int cursor_pos, int force_reset) {
    static int previous_lines_total = 0;  // Total lines occupied by previous input
    static int previous_term_width = 0;   // Terminal width from previous draw

    // Force reset when display has been disrupted
    if (force_reset) {
        previous_lines_total = 0;
        previous_term_width = 0;
    }

    int buffer_len = (int)strlen(buffer);
    int prompt_len = visible_strlen(prompt);
    int term_width = get_terminal_width();

    // Detect terminal resize - if width changed, we need to redraw from current position
    // because the line wrapping has changed and our saved position is invalid
    int terminal_resized = (previous_term_width != 0 && previous_term_width != term_width);

    // Move cursor up to start of previous input (unless terminal was resized or forced reset)
    if (previous_lines_total > 0 && !terminal_resized && !force_reset) {
        printf("\033[%dA", previous_lines_total);
    } else if (terminal_resized || force_reset) {
        // Terminal was resized or display disrupted: move to start of line
        // and clear everything from here down before redrawing
        printf("\r");
    }

    // Clear from here down and move to start of line
    printf("\r\033[J");

    // Print prompt and entire buffer once (using fwrite for performance with large pastes)
    printf("%s", prompt);
    if (buffer_len > 0) {
        fwrite(buffer, 1, (size_t)buffer_len, stdout);
    }

    // Calculate cursor position accounting for wrapping
    int cursor_line, cursor_col, total_lines;
    calculate_cursor_position(buffer, buffer_len, cursor_pos, prompt_len, term_width,
                             &cursor_line, &cursor_col, &total_lines);

    // Reposition cursor: move up from bottom, then position horizontally
    int lines_to_move_up = total_lines - cursor_line;
    if (lines_to_move_up > 0) {
        printf("\033[%dA", lines_to_move_up);
    }

    // Move to start of line, then to cursor column
    printf("\r");
    if (cursor_col > 0) {
        printf("\033[%dC", cursor_col);
    }

    fflush(stdout);

    // Store state for next redraw
    previous_lines_total = cursor_line;
    previous_term_width = term_width;
}

// Wrapper for normal redraw (no reset)
static void redraw_input_line(const char *prompt, const char *buffer, int cursor_pos) {
    redraw_input_line_internal(prompt, buffer, cursor_pos, 0);
}

// ============================================================================
// Input Queue Management (for ungetch-like functionality)
// ============================================================================

// Push a byte back into the input queue
static int queue_push(LineEditor *ed, unsigned char c) {
    if (ed->queue_count >= INPUT_QUEUE_SIZE) {
        return -1;  // Queue full
    }
    ed->input_queue[ed->queue_tail] = c;
    ed->queue_tail = (ed->queue_tail + 1) % INPUT_QUEUE_SIZE;
    ed->queue_count++;
    return 0;
}

// Pop a byte from the input queue
static int queue_pop(LineEditor *ed, unsigned char *c) {
    if (ed->queue_count <= 0) {
        return -1;  // Queue empty
    }
    *c = ed->input_queue[ed->queue_head];
    ed->queue_head = (ed->queue_head + 1) % INPUT_QUEUE_SIZE;
    ed->queue_count--;
    return 0;
}

// ============================================================================
// Input Reading with Timeout
// ============================================================================

// Read a single byte from stdin with timeout
// Returns: 1 on success, 0 on timeout, -1 on error/EOF
static int read_key_with_timeout(unsigned char *c, int timeout_ms) {
    fd_set readfds;
    struct timeval tv;
    int ret;

    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR) {
            return 0;  // Interrupted, treat as timeout
        }
        return -1;  // Error
    } else if (ret == 0) {
        return 0;  // Timeout
    }

    // Data available, read it
    if (read(STDIN_FILENO, c, 1) != 1) {
        return -1;  // Read error or EOF
    }

    return 1;  // Success
}

// Read a single byte, checking queue first
static int read_key(LineEditor *ed, unsigned char *c) {
    // Check queue first
    if (queue_pop(ed, c) == 0) {
        return 1;  // Got byte from queue
    }

    // Queue empty, read from stdin
    if (read(STDIN_FILENO, c, 1) != 1) {
        return -1;  // Error or EOF
    }

    return 1;  // Success
}

// ============================================================================
// Buffer Operations
// ============================================================================

// Insert character(s) at cursor position
// Returns: 0 on success, -1 on error (buffer full)
static int buffer_insert_char(LineEditor *ed, const unsigned char *utf8_char, int char_bytes) {
    if (ed->length + char_bytes >= (int)ed->buffer_capacity - 1) {
        return -1;  // Buffer full
    }

    // Make space for the new character(s)
    memmove(&ed->buffer[ed->cursor + char_bytes], &ed->buffer[ed->cursor],
            (size_t)(ed->length - ed->cursor + 1));

    // Copy the character bytes
    for (int i = 0; i < char_bytes; i++) {
        ed->buffer[ed->cursor + i] = (char)utf8_char[i];
    }

    ed->length += char_bytes;
    ed->cursor += char_bytes;
    return 0;
}

// Delete character at cursor position (forward delete)
// Returns: number of bytes deleted, or 0 if nothing to delete
static int buffer_delete_char(LineEditor *ed) {
    if (ed->cursor >= ed->length) {
        return 0;  // Nothing to delete
    }

    // Find the length of the UTF-8 character at cursor
    int char_len = utf8_char_length((unsigned char)ed->buffer[ed->cursor]);

    // Delete the character by moving subsequent text left
    memmove(&ed->buffer[ed->cursor],
           &ed->buffer[ed->cursor + char_len],
           (size_t)(ed->length - ed->cursor - char_len + 1));

    ed->length -= char_len;
    return char_len;
}

// Delete character before cursor (backspace)
// Returns: number of bytes deleted, or 0 if nothing to delete
static int buffer_backspace(LineEditor *ed) {
    if (ed->cursor <= 0) {
        return 0;  // Nothing to delete
    }

    // For simplicity, delete one byte at a time
    // (proper UTF-8 would require scanning backwards)
    memmove(&ed->buffer[ed->cursor - 1], &ed->buffer[ed->cursor],
            (size_t)(ed->length - ed->cursor + 1));
    ed->length--;
    ed->cursor--;
    return 1;
}

// Delete word before cursor (Alt+Backspace)
// Returns: number of bytes deleted, or 0 if nothing to delete
static int buffer_delete_word_backward(LineEditor *ed) {
    if (ed->cursor <= 0) {
        return 0;  // Nothing to delete
    }

    int word_start = ed->cursor - 1;

    // Skip trailing whitespace/punctuation (word boundaries)
    while (word_start > 0 && is_word_boundary(ed->buffer[word_start])) {
        word_start--;
    }

    // Skip the word characters (alphanumeric + underscore)
    while (word_start > 0 && !is_word_boundary(ed->buffer[word_start])) {
        word_start--;
    }

    // If we stopped at a boundary (not at start), move one forward
    if (word_start > 0 && is_word_boundary(ed->buffer[word_start])) {
        word_start++;
    }

    int delete_count = ed->cursor - word_start;
    if (delete_count > 0) {
        memmove(&ed->buffer[word_start], &ed->buffer[ed->cursor],
                (size_t)(ed->length - ed->cursor + 1));
        ed->length -= delete_count;
        ed->cursor = word_start;
    }

    return delete_count;
}

// Delete range of characters
// Returns: number of bytes deleted
static int buffer_delete_range(LineEditor *ed, int start, int end) {
    if (start < 0 || end > ed->length || start >= end) {
        return 0;  // Invalid range
    }

    int bytes_deleted = end - start;
    memmove(&ed->buffer[start], &ed->buffer[end], (size_t)(ed->length - end + 1));
    ed->length -= bytes_deleted;

    // Adjust cursor if needed
    if (ed->cursor > start) {
        if (ed->cursor < end) {
            ed->cursor = start;
        } else {
            ed->cursor -= bytes_deleted;
        }
    }

    return bytes_deleted;
}

// ============================================================================
// Paste Handling
// ============================================================================

/**
 * Handle completed paste - sanitize, optionally prompt, and insert into buffer
 * Returns: 1 if paste was accepted, 0 if rejected/error
 */
static int handle_paste_complete(LineEditor *ed, PasteState *paste_state,
                                 const char *prompt) {
    size_t paste_len = 0;
    const char *paste_content = paste_get_content(paste_state, &paste_len);

    if (!paste_content || paste_len == 0) {
        return 0;
    }

    // Create a copy for sanitization
    char *sanitized = malloc(paste_len + 1);
    if (!sanitized) {
        fprintf(stderr, "\nError: Failed to allocate memory for paste\n");
        return 0;
    }
    memcpy(sanitized, paste_content, paste_len);
    sanitized[paste_len] = '\0';

    // Sanitize with default options
    PasteSanitizeOptions opts = {
        .remove_control_chars = 1,
        .normalize_newlines = 1,
        .trim_whitespace = 1,
        .collapse_multiple_newlines = 1
    };
    size_t sanitized_len = paste_sanitize(sanitized, paste_len, &opts);

    // Count lines in pasted content
    size_t line_count = 0;
    for (size_t i = 0; i < sanitized_len; i++) {
        if (sanitized[i] == '\n') {
            line_count++;
        }
    }
    // If content doesn't end with newline, there's one more line
    if (sanitized_len > 0 && sanitized[sanitized_len - 1] != '\n') {
        line_count++;
    }

    // Decide whether to insert actual content for small pastes
    if (sanitized_len < 512) {
        // Small paste: insert actual sanitized content directly
        // Ensure buffer has enough capacity
        size_t needed_small = (size_t)ed->length + sanitized_len + 1;
        if (needed_small > ed->buffer_capacity) {
            size_t new_cap = ed->buffer_capacity;
            while (new_cap < needed_small) new_cap *= 2;
            char *new_buf = realloc(ed->buffer, new_cap);
            if (!new_buf) {
                fprintf(stderr, "\nError: Failed to expand buffer for small paste\n");
                free(sanitized);
                return 0;
            }
            ed->buffer = new_buf;
            ed->buffer_capacity = new_cap;
        }
        // Shift existing content to make room
        if (ed->cursor < ed->length) {
            memmove(&ed->buffer[(size_t)ed->cursor + sanitized_len],
                    &ed->buffer[ed->cursor],
                    (size_t)(ed->length - ed->cursor + 1));
        }
        // Copy sanitized content
        memcpy(&ed->buffer[ed->cursor], sanitized, sanitized_len);
        ed->cursor += (int)sanitized_len;
        ed->length += (int)sanitized_len;
        ed->buffer[ed->length] = '\0';
        // Cleanup
        free(sanitized);
        // Redraw line with inserted content
        redraw_input_line_internal(prompt, ed->buffer, ed->cursor, 0);
        return 1;
    }

    // Create placeholder text
    char placeholder[128];
    int placeholder_len;
    if (line_count > 1) {
        placeholder_len = snprintf(placeholder, sizeof(placeholder),
                                   "[pasted %zu lines, %zu chars]",
                                   line_count, sanitized_len);
    } else {
        placeholder_len = snprintf(placeholder, sizeof(placeholder),
                                   "[pasted %zu chars]", sanitized_len);
    }

    // Store the actual paste content for later submission
    free(ed->paste_content);  // Free any previous paste content
    ed->paste_content = sanitized;  // Transfer ownership
    ed->paste_content_len = sanitized_len;
    ed->paste_placeholder_start = ed->cursor;

    // Check if placeholder will fit in buffer
    size_t needed = (size_t)ed->length + (size_t)placeholder_len + 1;
    if (needed > ed->buffer_capacity) {
        // Expand buffer
        size_t new_capacity = ed->buffer_capacity;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }
        char *new_buffer = realloc(ed->buffer, new_capacity);
        if (!new_buffer) {
            fprintf(stderr, "\nError: Failed to expand buffer for placeholder\n");
            return 0;
        }
        ed->buffer = new_buffer;
        ed->buffer_capacity = new_capacity;
    }

    // Insert placeholder at cursor position
    if (ed->cursor < ed->length) {
        // Make room by shifting existing content
        memmove(&ed->buffer[(size_t)ed->cursor + (size_t)placeholder_len],
                &ed->buffer[ed->cursor],
                (size_t)(ed->length - ed->cursor + 1));
    }

    // Copy placeholder into buffer
    memcpy(&ed->buffer[ed->cursor], placeholder, (size_t)placeholder_len);
    ed->cursor += placeholder_len;
    ed->length += placeholder_len;
    ed->buffer[ed->length] = '\0';
    ed->paste_placeholder_len = placeholder_len;

    // Redraw line with placeholder
    redraw_input_line_internal(prompt, ed->buffer, ed->cursor, 0);

    return 1;
}

// ============================================================================
// API Implementation
// ============================================================================

void lineedit_init(LineEditor *ed, CompletionFn completer, void *ctx) {
    ed->buffer = malloc(INITIAL_BUFFER_SIZE);
    if (!ed->buffer) {
        LOG_ERROR("Failed to allocate line editor buffer");
        exit(1);
    }
    ed->buffer_capacity = INITIAL_BUFFER_SIZE;
    ed->buffer[0] = '\0';
    ed->cursor = 0;
    ed->length = 0;
    ed->completer = completer;
    ed->completer_ctx = ctx;
    history_init(&ed->history);
    // Initialize input queue
    ed->queue_head = 0;
    ed->queue_tail = 0;
    ed->queue_count = 0;
    // Initialize paste tracking
    ed->paste_content = NULL;
    ed->paste_content_len = 0;
    ed->paste_placeholder_start = 0;
    ed->paste_placeholder_len = 0;
}

void lineedit_free(LineEditor *ed) {
    free(ed->buffer);
    ed->buffer = NULL;
    ed->buffer_capacity = 0;
    ed->cursor = 0;
    ed->length = 0;
    history_free(&ed->history);
    // Free paste tracking
    free(ed->paste_content);
    ed->paste_content = NULL;
    ed->paste_content_len = 0;
}

void completion_free(CompletionResult *result) {
    if (!result) return;

    for (int i = 0; i < result->count; i++) {
        free(result->options[i]);
    }
    free(result->options);
    free(result);
}

char* lineedit_readline(LineEditor *ed, const char *prompt) {
    struct termios new_term;

    // Register cleanup handlers (only done once)
    register_cleanup_handlers();

    // Save terminal settings to global variable for signal handlers
    if (tcgetattr(STDIN_FILENO, &g_original_termios) < 0) {
        // Fall back to fgets if we can't set raw mode
        printf("%s", prompt);
        fflush(stdout);
        if (fgets(ed->buffer, (int)ed->buffer_capacity, stdin) == NULL) {
            return NULL;  // EOF
        }
        ed->buffer[strcspn(ed->buffer, "\n")] = 0;
        return strdup(ed->buffer);
    }

    // Set up raw mode
    new_term = g_original_termios;
    new_term.c_lflag &= (tcflag_t)~(ICANON | ECHO);  // Disable canonical mode and echo
    new_term.c_iflag &= (tcflag_t)~(ICRNL | INLCR);  // Don't translate CR<->NL so we can distinguish Enter from Ctrl+J
    new_term.c_cc[VMIN] = 1;
    new_term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
    g_terminal_modified = 1;  // Mark that terminal needs restoration

    // Enable bracketed paste mode
    printf("\033[?2004h");
    fflush(stdout);

    // Initialize buffer
    memset(ed->buffer, 0, ed->buffer_capacity);
    ed->length = 0;
    ed->cursor = 0;

    // Initialize paste handler
    PasteState *paste_state = paste_state_init();
    if (!paste_state) {
        restore_terminal();
        return NULL;
    }

    // Save current input when navigating history (so we can restore it)
    char *saved_input = NULL;

    // Print initial prompt
    printf("%s", prompt);
    fflush(stdout);

    int running = 1;
    while (running) {
        unsigned char c;

        // If in paste mode (timing-based), check if paste is complete
        if (paste_state->in_paste && paste_state->buffer_size > 0) {
            // Check if more data is available with a short timeout
            int ret = read_key_with_timeout(&c, PASTE_TIME_BURST_MS);
            if (ret <= 0) {
                // No more data - paste is complete
                paste_state->in_paste = 0;
                if (paste_state->buffer_size > 0) {
                    handle_paste_complete(ed, paste_state, prompt);
                    paste_state_reset(paste_state);
                }
                continue;
            }
            // More data available - continue with normal processing below
        } else {
            // Normal read (blocking)
            if (read_key(ed, &c) != 1) {
                // Error or EOF - cleanup handled by restore_terminal()
                restore_terminal();
                free(saved_input);
                paste_state_free(paste_state);
                return NULL;
            }
        }

        if (c == 27) {  // ESC sequence
            // If in timing-based paste mode, buffer the ESC and continue
            if (paste_state->in_paste) {
                paste_buffer_add_char(paste_state, (char)c);
                continue;
            }

            unsigned char seq[2];

            // Read next byte with timeout to distinguish standalone ESC from escape sequences
            int ret = read_key_with_timeout(&seq[0], 100);  // 100ms timeout
            if (ret <= 0) {
                // Timeout or error - treat as standalone ESC (ignore for now)
                continue;
            }

            if (seq[0] == 'b' || seq[0] == 'B') {
                // Alt+b: backward word
                ed->cursor = move_backward_word(ed->buffer, ed->cursor);
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            } else if (seq[0] == 'f' || seq[0] == 'F') {
                // Alt+f: forward word
                ed->cursor = move_forward_word(ed->buffer, ed->cursor, ed->length);
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            } else if (seq[0] == 'd' || seq[0] == 'D') {
                // Alt+d: delete next word
                if (delete_next_word(ed) > 0) {
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                }
            } else if (seq[0] == 127 || seq[0] == 8) {
                // Alt+Backspace: delete previous word
                if (buffer_delete_word_backward(ed) > 0) {
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                }
            } else if (seq[0] == '[') {
                // Arrow keys and other escape sequences
                ret = read_key_with_timeout(&seq[1], 100);  // 100ms timeout
                if (ret <= 0) {
                    continue;
                }

                if (seq[1] == '2') {
                    // Possible bracketed paste sequence: \e[200~ or \e[201~
                    unsigned char paste_seq[3];
                    int paste_bytes_read = 0;
                    // Read 3 more bytes for the paste sequence
                    for (int i = 0; i < 3; i++) {
                        ret = read_key_with_timeout(&paste_seq[i], 100);
                        if (ret <= 0) break;
                        paste_bytes_read++;
                    }
                    if (paste_bytes_read == 3 &&
                        paste_seq[0] == '0' && paste_seq[1] == '0' && paste_seq[2] == '~') {
                        // Start of paste - enter paste collection mode
                        paste_state->in_paste = 1;
                        paste_state->buffer_size = 0;
                    } else if (paste_bytes_read == 3 &&
                               paste_seq[0] == '0' && paste_seq[1] == '1' && paste_seq[2] == '~') {
                        // End of paste - process the collected content
                        paste_state->in_paste = 0;
                        if (paste_state->buffer_size > 0) {
                            handle_paste_complete(ed, paste_state, prompt);
                            paste_state_reset(paste_state);
                        }
                    } else {
                        // Not a valid paste sequence - push bytes back to queue
                        for (int i = paste_bytes_read - 1; i >= 0; i--) {
                            queue_push(ed, paste_seq[i]);
                        }
                    }
                } else if (seq[1] == '3') {
                    // Delete key: \e[3~
                    unsigned char tilde;
                    ret = read_key_with_timeout(&tilde, 100);
                    if (ret > 0 && tilde == '~') {
                        // Delete character at cursor (forward delete)
                        if (buffer_delete_char(ed) > 0) {
                            redraw_input_line(prompt, ed->buffer, ed->cursor);
                        }
                    } else if (ret > 0) {
                        // Not a tilde - push it back to queue
                        queue_push(ed, tilde);
                    }
                } else if (seq[1] == 'D') {
                    // Left arrow
                    if (ed->cursor > 0) {
                        ed->cursor--;
                        printf("\033[D");  // Move cursor left
                        fflush(stdout);
                    }
                } else if (seq[1] == 'C') {
                    // Right arrow
                    if (ed->cursor < ed->length) {
                        ed->cursor++;
                        printf("\033[C");  // Move cursor right
                        fflush(stdout);
                    }
                } else if (seq[1] == 'A') {
                    // Up arrow - previous history entry
                    if (ed->history.count > 0) {
                        // Save current input if this is the first Up press
                        if (ed->history.position == -1) {
                            free(saved_input);
                            saved_input = strdup(ed->buffer);
                            ed->history.position = ed->history.count;
                        }

                        // Navigate to previous entry
                        if (ed->history.position > 0) {
                            ed->history.position--;
                            const char *hist_entry = ed->history.entries[ed->history.position];
                            strncpy(ed->buffer, hist_entry, ed->buffer_capacity - 1);
                            ed->buffer[ed->buffer_capacity - 1] = '\0';
                            ed->length = (int)strlen(ed->buffer);
                            ed->cursor = ed->length;
                            redraw_input_line(prompt, ed->buffer, ed->cursor);
                        }
                    }
                } else if (seq[1] == 'B') {
                    // Down arrow - next history entry
                    if (ed->history.position != -1) {
                        ed->history.position++;

                        if (ed->history.position >= ed->history.count) {
                            // Restore saved input
                            if (saved_input) {
                                strncpy(ed->buffer, saved_input, ed->buffer_capacity - 1);
                                ed->buffer[ed->buffer_capacity - 1] = '\0';
                                ed->length = (int)strlen(ed->buffer);
                                ed->cursor = ed->length;
                            } else {
                                ed->buffer[0] = '\0';
                                ed->length = 0;
                                ed->cursor = 0;
                            }
                            ed->history.position = -1;
                        } else {
                            // Show next entry
                            const char *hist_entry = ed->history.entries[ed->history.position];
                            strncpy(ed->buffer, hist_entry, ed->buffer_capacity - 1);
                            ed->buffer[ed->buffer_capacity - 1] = '\0';
                            ed->length = (int)strlen(ed->buffer);
                            ed->cursor = ed->length;
                        }
                        redraw_input_line(prompt, ed->buffer, ed->cursor);
                    }
                } else if (seq[1] == 'H') {
                    // Home
                    ed->cursor = 0;
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                } else if (seq[1] == 'F') {
                    // End
                    ed->cursor = ed->length;
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                } else {
                    // Unrecognized escape sequence - push seq[1] back to queue
                    queue_push(ed, seq[1]);
                }
            } else {
                // Unrecognized ESC+char sequence - push seq[0] back to queue
                queue_push(ed, seq[0]);
            }
        } else if (c == 1) {
            // Ctrl+A: beginning of line
            ed->cursor = 0;
            redraw_input_line(prompt, ed->buffer, ed->cursor);
        } else if (c == 5) {
            // Ctrl+E: end of line
            ed->cursor = ed->length;
            redraw_input_line(prompt, ed->buffer, ed->cursor);
        } else if (c == 4) {
            // Ctrl+D: EOF only (always exits, even if buffer has content)
            printf("\n");
            restore_terminal();
            free(saved_input);
            return NULL;
        } else if (c == 11) {
            // Ctrl+K: kill to end of line
            ed->buffer[ed->cursor] = 0;
            ed->length = ed->cursor;
            redraw_input_line(prompt, ed->buffer, ed->cursor);
        } else if (c == 21) {
            // Ctrl+U: kill to beginning of line
            if (ed->cursor > 0) {
                memmove(ed->buffer, &ed->buffer[ed->cursor], (size_t)(ed->length - ed->cursor + 1));
                ed->length -= ed->cursor;
                ed->cursor = 0;
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            }
        } else if (c == 12) {
            // Ctrl+L: clear entire input box
            ed->buffer[0] = '\0';
            ed->length = 0;
            ed->cursor = 0;
            redraw_input_line(prompt, ed->buffer, ed->cursor);
        } else if (c == 127 || c == 8) {
            // Backspace
            if (buffer_backspace(ed) > 0) {
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            }
        } else if (c == '\n') {
            // Ctrl+J (ASCII 10): Insert newline character for multiline input
            if (paste_state->in_paste) {
                // In paste mode, buffer the newline
                paste_buffer_add_char(paste_state, (char)c);
            } else {
                // Normal mode: Ctrl+J inserts newline
                unsigned char newline = '\n';
                if (buffer_insert_char(ed, &newline, 1) == 0) {
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                }
            }
        } else if (c == '\r') {
            // Enter key (ASCII 13): Submit input
            if (paste_state->in_paste) {
                // In paste mode, buffer the carriage return (will be sanitized later)
                paste_buffer_add_char(paste_state, (char)c);
            } else {
                // Normal mode: submit input
                // If there's a paste placeholder, replace it with the actual content
                if (ed->paste_content && ed->paste_content_len > 0) {
                    // Calculate new buffer size needed
                    size_t new_len = (size_t)ed->length - (size_t)ed->paste_placeholder_len + ed->paste_content_len;

                    // Ensure buffer is large enough
                    if (new_len + 1 > ed->buffer_capacity) {
                        size_t new_capacity = ed->buffer_capacity;
                        while (new_capacity < new_len + 1) {
                            new_capacity *= 2;
                        }
                        char *new_buffer = realloc(ed->buffer, new_capacity);
                        if (!new_buffer) {
                            fprintf(stderr, "\nError: Failed to expand buffer for paste submission\n");
                            printf("\n");
                            running = 0;
                            continue;
                        }
                        ed->buffer = new_buffer;
                        ed->buffer_capacity = new_capacity;
                    }

                    // Calculate positions
                    size_t placeholder_start = (size_t)ed->paste_placeholder_start;
                    int placeholder_end = ed->paste_placeholder_start + ed->paste_placeholder_len;
                    int tail_len = ed->length - placeholder_end;

                    // Move tail if necessary
                    if (tail_len > 0) {
                        memmove(&ed->buffer[placeholder_start + ed->paste_content_len],
                                &ed->buffer[placeholder_end],
                                (size_t)tail_len);
                    }

                    // Replace placeholder with actual content
                    memcpy(&ed->buffer[placeholder_start],
                           ed->paste_content,
                           ed->paste_content_len);

                    // Update length
                    ed->length = (int)new_len;
                    ed->buffer[ed->length] = '\0';

                    // Clear paste tracking
                    free(ed->paste_content);
                    ed->paste_content = NULL;
                    ed->paste_content_len = 0;
                    ed->paste_placeholder_start = 0;
                    ed->paste_placeholder_len = 0;
                }

                printf("\n");
                running = 0;
            }
        } else if (c == '\t') {
            // Tab handling
            if (paste_state->in_paste) {
                // In paste mode, buffer the tab
                paste_buffer_add_char(paste_state, (char)c);
            } else if (ed->completer) {
                // Normal mode: tab completion
                CompletionResult *res = ed->completer(ed->buffer, ed->cursor, ed->completer_ctx);
                if (!res || res->count == 0) {
                    // No completions, beep
                    printf("\a");
                    fflush(stdout);
                    if (res) completion_free(res);
                } else if (res->count == 1) {
                    // Single completion: replace current word
                    const char *opt = res->options[0];
                    int optlen = (int)strlen(opt);
                    // Find start of current word
                    int start = ed->cursor - 1;
                    while (start >= 0 && ed->buffer[start] != ' ' && ed->buffer[start] != '\t') {
                        start--;
                    }
                    start++;
                    int tail_len = ed->length - ed->cursor;
                    size_t needed = (size_t)(start + optlen + tail_len + 1);
                    if (needed > ed->buffer_capacity) {
                        ed->buffer = realloc(ed->buffer, needed);
                        ed->buffer_capacity = needed;
                    }
                    // Move tail
                    memmove(ed->buffer + start + optlen, ed->buffer + ed->cursor, (size_t)(tail_len + 1));
                    // Copy completion
                    memcpy(ed->buffer + start, opt, (size_t)optlen);
                    ed->cursor = start + optlen;
                    ed->length = start + optlen + tail_len;
                    completion_free(res);
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                } else {
                    // Multiple completions: list them, then redraw
                    printf("\n");
                    for (int i = 0; i < res->count; i++) {
                        printf("%s\t\n", res->options[i]);
                    }
                    completion_free(res);
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                }
            } else {
                printf("\a"); fflush(stdout);
            }
        } else if (c >= 32) {
            // Printable character (ASCII or UTF-8)

            // Always check for rapid input timing (tmux paste, Ctrl+Shift+V, etc.)
            // This updates the timing state even if not yet in paste mode
            int paste_detected = detect_paste_by_timing(paste_state);

            if (!paste_state->in_paste && paste_detected) {
                // Fast input detected - enter paste mode
                paste_state->in_paste = 1;
                paste_state->buffer_size = 0;
            }

            // If in paste mode, buffer character instead of inserting
            if (paste_state->in_paste) {
                if (paste_buffer_add_char(paste_state, (char)c) < 0) {
                    // Buffer overflow
                    fprintf(stderr, "\n\033[31mError: Paste buffer overflow (>1MB)\033[0m\n");
                    paste_state_reset(paste_state);
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                }
                continue;
            }

            unsigned char utf8_buffer[4];
            int char_bytes = 0;

            // Check if this is a UTF-8 multibyte character
            if (c >= 128) {
                char_bytes = read_utf8_char(utf8_buffer, c);
                if (char_bytes == 0) {
                    // Read error, skip this character
                    continue;
                }
            } else if (c >= 32 && c < 127) {
                // ASCII printable character
                utf8_buffer[0] = c;
                char_bytes = 1;
            } else {
                // Control character, skip
                continue;
            }

            // Insert character using buffer operation
            if (buffer_insert_char(ed, utf8_buffer, char_bytes) == 0) {
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            }
        }
    }

    // Restore terminal to normal state
    restore_terminal();

    // Add to history (if not empty)
    if (ed->buffer[0] != '\0') {
        history_add(&ed->history, ed->buffer);
    }

    // Cleanup
    free(saved_input);
    paste_state_free(paste_state);

    return strdup(ed->buffer);
}
