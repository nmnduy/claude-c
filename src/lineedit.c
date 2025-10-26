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

#define INITIAL_BUFFER_SIZE 4096

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

// Redraw the input line with cursor at correct position
// Handles multiline input by displaying newlines as actual line breaks
static void redraw_input_line(const char *prompt, const char *buffer, int cursor_pos) {
    static int previous_cursor_line = 0;  // Which line (0-indexed) the cursor was on

    int buffer_len = strlen(buffer);
    int prompt_len = visible_strlen(prompt);

    // Move cursor up to start of previous input
    // Cursor was left on previous_cursor_line, so move up by that amount
    if (previous_cursor_line > 0) {
        printf("\033[%dA", previous_cursor_line);
    }

    // Clear from here down and move to start of line
    printf("\r\033[J");

    // Print prompt and entire buffer once
    printf("%s", prompt);
    for (int i = 0; i < buffer_len; i++) {
        putchar(buffer[i]);  // Prints \n naturally
    }

    // Calculate cursor position in the buffer (column within current line)
    // Also track which line (0-indexed) the cursor is on
    int col_position = 0;
    int cursor_on_first_line = 1;
    int cursor_line = 0;  // 0 = first line, 1 = second line, etc.
    for (int i = 0; i < cursor_pos; i++) {
        if (buffer[i] == '\n') {
            col_position = 0;
            cursor_on_first_line = 0;  // We've seen a newline, so not on first line
            cursor_line++;
        } else {
            col_position++;
        }
    }

    // Calculate lines after cursor
    int lines_after_cursor = 0;
    for (int i = cursor_pos; i < buffer_len; i++) {
        if (buffer[i] == '\n') lines_after_cursor++;
    }

    // Reposition cursor: move up to cursor's line, then position horizontally
    if (lines_after_cursor > 0) {
        printf("\033[%dA", lines_after_cursor);
    }
    printf("\r");  // Start of line

    // Move right to cursor position
    // Only add prompt length if cursor is on the first line
    int target_col = (cursor_on_first_line ? prompt_len : 0) + col_position;
    if (target_col > 0) {
        printf("\033[%dC", target_col);
    }

    fflush(stdout);

    // Store cursor line for next redraw
    previous_cursor_line = cursor_line;
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
    struct termios old_term, new_term;

    // Save terminal settings
    if (tcgetattr(STDIN_FILENO, &old_term) < 0) {
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
    new_term = old_term;
    new_term.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    new_term.c_cc[VMIN] = 1;
    new_term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

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
            // Error or EOF
            printf("\033[?2004l");  // Disable bracketed paste mode
            fflush(stdout);
            tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
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
            printf("\033[?2004l");  // Disable bracketed paste mode
            fflush(stdout);
            tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
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

    // Disable bracketed paste mode
    printf("\033[?2004l");
    fflush(stdout);

    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);

    return strdup(ed->buffer);
}
