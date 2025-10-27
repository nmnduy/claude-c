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

#define INITIAL_BUFFER_SIZE 4096

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

// Delete the next word
static int delete_next_word(char *buffer, int *cursor_pos, int *buffer_len) {
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
}

void lineedit_free(LineEditor *ed) {
    free(ed->buffer);
    ed->buffer = NULL;
    ed->buffer_capacity = 0;
    ed->cursor = 0;
    ed->length = 0;
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

    // Print initial prompt
    printf("%s", prompt);
    fflush(stdout);

    int running = 1;
    while (running) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) {
            // Error or EOF - cleanup handled by restore_terminal()
            restore_terminal();
            return NULL;
        }

        if (c == 27) {  // ESC sequence
            unsigned char seq[2];

            // Read next two bytes
            if (read(STDIN_FILENO, &seq[0], 1) != 1) {
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
                if (delete_next_word(ed->buffer, &ed->cursor, &ed->length)) {
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                }
            } else if (seq[0] == '[') {
                // Arrow keys and other escape sequences
                if (read(STDIN_FILENO, &seq[1], 1) != 1) {
                    continue;
                }

                if (seq[1] == '2') {
                    // Possible bracketed paste sequence: \e[200~ or \e[201~
                    unsigned char paste_seq[3];
                    if (read(STDIN_FILENO, paste_seq, 3) == 3) {
                        if (paste_seq[0] == '0' && paste_seq[1] == '0' && paste_seq[2] == '~') {
                            in_paste_mode = 1;  // Start of paste
                        } else if (paste_seq[0] == '0' && paste_seq[1] == '1' && paste_seq[2] == '~') {
                            in_paste_mode = 0;  // End of paste
                        }
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
                } else if (seq[1] == 'H') {
                    // Home
                    ed->cursor = 0;
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                } else if (seq[1] == 'F') {
                    // End
                    ed->cursor = ed->length;
                    redraw_input_line(prompt, ed->buffer, ed->cursor);
                }
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
            if (ed->cursor > 0) {
                memmove(&ed->buffer[ed->cursor - 1], &ed->buffer[ed->cursor], ed->length - ed->cursor + 1);
                ed->length--;
                ed->cursor--;
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            }
        } else if (c == 14) {
            // Ctrl+N (ASCII 14): Insert newline character
            if (ed->length < (int)ed->buffer_capacity - 1) {
                memmove(&ed->buffer[ed->cursor + 1], &ed->buffer[ed->cursor], ed->length - ed->cursor + 1);
                ed->buffer[ed->cursor] = '\n';
                ed->length++;
                ed->cursor++;
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            }
        } else if (c == '\r' || c == '\n') {
            // Enter: Submit, unless we're in paste mode
            if (in_paste_mode) {
                // In paste mode, insert newline as a regular character
                if (ed->length < (int)ed->buffer_capacity - 1) {
                    memmove(&ed->buffer[ed->cursor + 1], &ed->buffer[ed->cursor], ed->length - ed->cursor + 1);
                    ed->buffer[ed->cursor] = '\n';
                    ed->length++;
                    ed->cursor++;
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
        } else if (c >= 32 && c < 127) {
            // Printable character
            if (ed->length < (int)ed->buffer_capacity - 1) {
                // Insert character at cursor position
                memmove(&ed->buffer[ed->cursor + 1], &ed->buffer[ed->cursor], ed->length - ed->cursor + 1);
                ed->buffer[ed->cursor] = c;
                ed->length++;
                ed->cursor++;
                redraw_input_line(prompt, ed->buffer, ed->cursor);
            }
        }
    }

    // Restore terminal to normal state
    restore_terminal();

    return strdup(ed->buffer);
}
