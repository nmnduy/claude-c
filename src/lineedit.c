/*
 * lineedit.c - Generic Line Editor Implementation
 *
 * Extracted from claude.c to provide reusable line editing functionality
 */

#include "lineedit.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/select.h>
#include <errno.h>

#define INITIAL_BUFFER_SIZE 4096
#define DEFAULT_HISTORY_SIZE 100

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

// Signal handler for Ctrl+C, SIGTERM, etc.
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
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // kill command
    signal(SIGHUP, signal_handler);   // Terminal hangup
    signal(SIGQUIT, signal_handler);  // Ctrl+\

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
static int is_word_boundary(char c) {
    return !isalnum(c) && c != '_';
}

// Move cursor backward by one word
static int move_backward_word(const char *buffer, int cursor_pos) {
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
static int move_forward_word(const char *buffer, int cursor_pos, int buffer_len) {
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
static int visible_strlen(const char *str) {
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
static void redraw_input_line(const char *prompt, const char *buffer, int cursor_pos) {
    static int previous_lines_total = 0;  // Total lines occupied by previous input
    static int previous_term_width = 0;   // Terminal width from previous draw

    int buffer_len = (int)strlen(buffer);
    int prompt_len = visible_strlen(prompt);
    int term_width = get_terminal_width();

    // Detect terminal resize - if width changed, we need to redraw from current position
    // because the line wrapping has changed and our saved position is invalid
    int terminal_resized = (previous_term_width != 0 && previous_term_width != term_width);

    // Move cursor up to start of previous input (unless terminal was resized)
    if (previous_lines_total > 0 && !terminal_resized) {
        printf("\033[%dA", previous_lines_total);
    } else if (terminal_resized) {
        // Terminal was resized: can't rely on old position, so move to start of line
        // and clear everything from here down before redrawing
        printf("\r");
    }

    // Clear from here down and move to start of line
    printf("\r\033[J");

    // Print prompt and entire buffer once
    printf("%s", prompt);
    for (int i = 0; i < buffer_len; i++) {
        putchar(buffer[i]);  // Prints \n naturally
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
            ed->length - ed->cursor + 1);

    // Copy the character bytes
    for (int i = 0; i < char_bytes; i++) {
        ed->buffer[ed->cursor + i] = utf8_char[i];
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
           ed->length - ed->cursor - char_len + 1);

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
            ed->length - ed->cursor + 1);
    ed->length--;
    ed->cursor--;
    return 1;
}

// Delete range of characters
// Returns: number of bytes deleted
static int buffer_delete_range(LineEditor *ed, int start, int end) {
    if (start < 0 || end > ed->length || start >= end) {
        return 0;  // Invalid range
    }

    int bytes_deleted = end - start;
    memmove(&ed->buffer[start], &ed->buffer[end], ed->length - end + 1);
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
}

void lineedit_free(LineEditor *ed) {
    free(ed->buffer);
    ed->buffer = NULL;
    ed->buffer_capacity = 0;
    ed->cursor = 0;
    ed->length = 0;
    history_free(&ed->history);
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
        if (fgets(ed->buffer, ed->buffer_capacity, stdin) == NULL) {
            return NULL;  // EOF
        }
        ed->buffer[strcspn(ed->buffer, "\n")] = 0;
        return strdup(ed->buffer);
    }

    // Set up raw mode
    new_term = g_original_termios;
    new_term.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
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
    int in_paste_mode = 0;  // Track if we're receiving pasted text

    // Save current input when navigating history (so we can restore it)
    char *saved_input = NULL;

    // Print initial prompt
    printf("%s", prompt);
    fflush(stdout);

    int running = 1;
    while (running) {
        unsigned char c;
        if (read_key(ed, &c) != 1) {
            // Error or EOF - cleanup handled by restore_terminal()
            restore_terminal();
            free(saved_input);
            return NULL;
        }

        if (c == 27) {  // ESC sequence
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
                        in_paste_mode = 1;  // Start of paste
                    } else if (paste_bytes_read == 3 &&
                               paste_seq[0] == '0' && paste_seq[1] == '1' && paste_seq[2] == '~') {
                        in_paste_mode = 0;  // End of paste
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
                memmove(ed->buffer, &ed->buffer[ed->cursor], ed->length - ed->cursor + 1);
                ed->length -= ed->cursor;
                ed->cursor = 0;
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            }
        } else if (c == 127 || c == 8) {
            // Backspace
            if (buffer_backspace(ed) > 0) {
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            }
        } else if (c == 14) {
            // Ctrl+N (ASCII 14): Insert newline character
            unsigned char newline = '\n';
            if (buffer_insert_char(ed, &newline, 1) == 0) {
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            }
        } else if (c == '\r' || c == '\n') {
            // Enter: Submit, unless we're in paste mode
            if (in_paste_mode) {
                // In paste mode, insert newline as a regular character
                unsigned char newline = '\n';
                if (buffer_insert_char(ed, &newline, 1) == 0) {
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                }
            } else {
                // Normal mode: submit on Enter
                printf("\n");
                running = 0;
            }
                } else if (c == '\t') {
            // Tab completion
            if (ed->completer) {
                CompletionResult *res = ed->completer(ed->buffer, ed->cursor, ed->completer_ctx);
                if (!res || res->count == 0) {
                    // No completions, beep
                    printf("\a");
                    fflush(stdout);
                    if (res) completion_free(res);
                } else if (res->count == 1) {
                    // Single completion: replace current word
                    const char *opt = res->options[0];
                    int optlen = strlen(opt);
                    // Find start of current word
                    int start = ed->cursor - 1;
                    while (start >= 0 && ed->buffer[start] != ' ' && ed->buffer[start] != '\t') {
                        start--;
                    }
                    start++;
                    int tail_len = ed->length - ed->cursor;
                    size_t needed = start + optlen + tail_len + 1;
                    if (needed > ed->buffer_capacity) {
                        ed->buffer = realloc(ed->buffer, needed);
                        ed->buffer_capacity = needed;
                    }
                    // Move tail
                    memmove(ed->buffer + start + optlen, ed->buffer + ed->cursor, tail_len + 1);
                    // Copy completion
                    memcpy(ed->buffer + start, opt, optlen);
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

    return strdup(ed->buffer);
}
