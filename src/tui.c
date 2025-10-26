/*
 * TUI (Terminal User Interface) - ncurses-based interface
 */

#include "tui.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define INITIAL_CONV_CAPACITY 1000
#define INPUT_BUFFER_SIZE 8192

// Helper: Duplicate a string
static char* str_dup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, str, len + 1);
    }
    return dup;
}

// Helper: Word-wrap text to fit within specified width
// Returns array of lines (caller must free each line and the array)
static char** word_wrap(const char *text, int width, int *line_count) {
    *line_count = 0;
    if (!text || width <= 0) return NULL;

    int capacity = 10;
    char **lines = malloc(capacity * sizeof(char*));
    if (!lines) return NULL;

    const char *p = text;
    while (*p) {
        // Find end of line (natural newline or width limit)
        const char *line_start = p;
        const char *last_space = NULL;
        int col = 0;

        while (*p && *p != '\n') {
            if (*p == ' ' || *p == '\t') {
                last_space = p;
            }
            col++;
            if (col >= width) {
                // Need to wrap
                if (last_space && last_space > line_start) {
                    // Break at last space
                    p = last_space + 1;
                } else {
                    // No space found, hard break
                }
                break;
            }
            p++;
        }

        // Extract line
        int line_len = p - line_start;
        if (*p == '\n') {
            p++; // Skip newline
        }

        // Trim trailing spaces from line
        while (line_len > 0 && (line_start[line_len-1] == ' ' || line_start[line_len-1] == '\t')) {
            line_len--;
        }

        // Allocate and store line
        if (*line_count >= capacity) {
            capacity *= 2;
            char **new_lines = realloc(lines, capacity * sizeof(char*));
            if (!new_lines) {
                for (int i = 0; i < *line_count; i++) {
                    free(lines[i]);
                }
                free(lines);
                return NULL;
            }
            lines = new_lines;
        }

        lines[*line_count] = malloc(line_len + 1);
        if (!lines[*line_count]) {
            for (int i = 0; i < *line_count; i++) {
                free(lines[i]);
            }
            free(lines);
            return NULL;
        }
        memcpy(lines[*line_count], line_start, line_len);
        lines[*line_count][line_len] = '\0';
        (*line_count)++;
    }

    // If no lines were created, create one empty line
    if (*line_count == 0) {
        lines[0] = str_dup("");
        *line_count = 1;
    }

    return lines;
}

int tui_init(TUIState *tui) {
    if (!tui) return -1;

    // Initialize ncurses
    initscr();
    cbreak();              // Disable line buffering
    noecho();              // Don't echo input
    keypad(stdscr, TRUE);  // Enable function keys
    curs_set(1);           // Show cursor

    // Initialize colors if available
    if (has_colors()) {
        start_color();
        use_default_colors();

        // Define color pairs
        init_pair(COLOR_PAIR_DEFAULT, -1, -1);           // Default colors
        init_pair(COLOR_PAIR_USER, COLOR_GREEN, -1);     // Green for user
        init_pair(COLOR_PAIR_ASSISTANT, COLOR_BLUE, -1); // Blue for assistant
        init_pair(COLOR_PAIR_TOOL, COLOR_YELLOW, -1);    // Yellow for tools
        init_pair(COLOR_PAIR_ERROR, COLOR_RED, -1);      // Red for errors
        init_pair(COLOR_PAIR_STATUS, COLOR_CYAN, -1);    // Cyan for status
        init_pair(COLOR_PAIR_PROMPT, COLOR_GREEN, -1);   // Green for prompt
    }

    // Get screen dimensions
    getmaxyx(stdscr, tui->screen_height, tui->screen_width);

    // Calculate window heights
    tui->input_height = 4;  // Fixed height for input
    tui->conv_height = tui->screen_height - tui->input_height - 1; // -1 for status line

    // Create windows
    tui->conv_win = newwin(tui->conv_height, tui->screen_width, 0, 0);
    tui->status_win = newwin(1, tui->screen_width, tui->conv_height, 0);
    tui->input_win = newwin(tui->input_height, tui->screen_width,
                            tui->conv_height + 1, 0);

    if (!tui->conv_win || !tui->status_win || !tui->input_win) {
        tui_cleanup(tui);
        return -1;
    }

    // Enable scrolling for conversation window
    scrollok(tui->conv_win, TRUE);
    idlok(tui->conv_win, TRUE);

    // Initialize conversation buffer
    tui->conv_lines_capacity = INITIAL_CONV_CAPACITY;
    tui->conv_lines = malloc(tui->conv_lines_capacity * sizeof(char*));
    if (!tui->conv_lines) {
        tui_cleanup(tui);
        return -1;
    }
    tui->conv_lines_count = 0;
    tui->conv_scroll_offset = 0;

    tui->is_initialized = 1;

    // Initial refresh
    tui_refresh(tui);

    return 0;
}

void tui_cleanup(TUIState *tui) {
    if (!tui) return;

    // Free conversation lines
    if (tui->conv_lines) {
        for (int i = 0; i < tui->conv_lines_count; i++) {
            free(tui->conv_lines[i]);
        }
        free(tui->conv_lines);
        tui->conv_lines = NULL;
    }

    // Destroy windows
    if (tui->conv_win) delwin(tui->conv_win);
    if (tui->status_win) delwin(tui->status_win);
    if (tui->input_win) delwin(tui->input_win);

    // End ncurses
    if (tui->is_initialized) {
        endwin();
    }

    tui->is_initialized = 0;
}

void tui_add_conversation_line(TUIState *tui, const char *prefix, const char *text, TUIColorPair color_pair) {
    if (!tui || !tui->is_initialized) return;

    // Build full line with prefix
    char full_line[8192];
    if (prefix) {
        snprintf(full_line, sizeof(full_line), "%s %s", prefix, text);
    } else {
        snprintf(full_line, sizeof(full_line), "%s", text);
    }

    // Word-wrap the text to fit window width
    int wrapped_count = 0;
    char **wrapped_lines = word_wrap(full_line, tui->screen_width - 2, &wrapped_count);

    if (!wrapped_lines) return;

    // Add each wrapped line to conversation buffer
    for (int i = 0; i < wrapped_count; i++) {
        // Expand buffer if needed
        if (tui->conv_lines_count >= tui->conv_lines_capacity) {
            int new_capacity = tui->conv_lines_capacity * 2;
            char **new_lines = realloc(tui->conv_lines, new_capacity * sizeof(char*));
            if (!new_lines) {
                // Out of memory, skip this line
                free(wrapped_lines[i]);
                continue;
            }
            tui->conv_lines = new_lines;
            tui->conv_lines_capacity = new_capacity;
        }

        tui->conv_lines[tui->conv_lines_count++] = wrapped_lines[i];
    }
    free(wrapped_lines);

    // Redraw conversation window
    werase(tui->conv_win);

    // Calculate which lines to display (last conv_height lines, with scroll offset)
    int start_line = tui->conv_lines_count - tui->conv_height + tui->conv_scroll_offset;
    if (start_line < 0) start_line = 0;

    int end_line = start_line + tui->conv_height;
    if (end_line > tui->conv_lines_count) end_line = tui->conv_lines_count;

    // Draw lines
    for (int i = start_line; i < end_line; i++) {
        // Determine color based on line content
        TUIColorPair line_color = COLOR_PAIR_DEFAULT;
        if (strstr(tui->conv_lines[i], "[User]")) {
            line_color = COLOR_PAIR_USER;
        } else if (strstr(tui->conv_lines[i], "[Assistant]")) {
            line_color = COLOR_PAIR_ASSISTANT;
        } else if (strstr(tui->conv_lines[i], "[Tool:")) {
            line_color = COLOR_PAIR_TOOL;
        } else if (strstr(tui->conv_lines[i], "[Error]")) {
            line_color = COLOR_PAIR_ERROR;
        } else if (strstr(tui->conv_lines[i], "[Status]")) {
            line_color = COLOR_PAIR_STATUS;
        }

        wattron(tui->conv_win, COLOR_PAIR(line_color));
        mvwprintw(tui->conv_win, i - start_line, 0, "%s", tui->conv_lines[i]);
        wattroff(tui->conv_win, COLOR_PAIR(line_color));
    }

    wrefresh(tui->conv_win);
}

void tui_update_status(TUIState *tui, const char *status_text) {
    if (!tui || !tui->is_initialized || !tui->status_win) return;

    werase(tui->status_win);
    wattron(tui->status_win, COLOR_PAIR(COLOR_PAIR_STATUS) | A_REVERSE);

    // Fill entire status line
    for (int i = 0; i < tui->screen_width; i++) {
        mvwaddch(tui->status_win, 0, i, ' ');
    }

    // Print status text
    mvwprintw(tui->status_win, 0, 1, "%s", status_text);
    wattroff(tui->status_win, COLOR_PAIR(COLOR_PAIR_STATUS) | A_REVERSE);

    wrefresh(tui->status_win);
}

char* tui_read_input(TUIState *tui, const char *prompt) {
    if (!tui || !tui->is_initialized || !tui->input_win) return NULL;

    // Clear input window and draw prompt
    werase(tui->input_win);
    box(tui->input_win, 0, 0);

    wattron(tui->input_win, COLOR_PAIR(COLOR_PAIR_PROMPT));
    mvwprintw(tui->input_win, 1, 2, "%s", prompt);
    wattroff(tui->input_win, COLOR_PAIR(COLOR_PAIR_PROMPT));

    wrefresh(tui->input_win);

    // Allocate input buffer
    char *buffer = malloc(INPUT_BUFFER_SIZE);
    if (!buffer) return NULL;

    int buf_len = 0;
    int cursor_pos = 0;
    buffer[0] = '\0';

    // Cursor position in window
    int prompt_len = strlen(prompt);
    int base_x = 2 + prompt_len + 1; // 2 (border + space) + prompt + space
    int base_y = 1;

    wmove(tui->input_win, base_y, base_x);
    wrefresh(tui->input_win);

    int done = 0;
    while (!done) {
        int ch = wgetch(tui->input_win);

        switch (ch) {
            case KEY_RESIZE:
                // Handle terminal resize
                tui_handle_resize(tui);
                free(buffer);
                return NULL;

            case 4: // Ctrl+D (EOF)
                free(buffer);
                return NULL;

            case '\n':
            case KEY_ENTER:
                done = 1;
                break;

            case KEY_BACKSPACE:
            case 127:
            case 8:
                if (cursor_pos > 0) {
                    // Remove character before cursor
                    memmove(&buffer[cursor_pos - 1], &buffer[cursor_pos], buf_len - cursor_pos + 1);
                    buf_len--;
                    cursor_pos--;

                    // Redraw
                    werase(tui->input_win);
                    box(tui->input_win, 0, 0);
                    wattron(tui->input_win, COLOR_PAIR(COLOR_PAIR_PROMPT));
                    mvwprintw(tui->input_win, base_y, 2, "%s", prompt);
                    wattroff(tui->input_win, COLOR_PAIR(COLOR_PAIR_PROMPT));
                    mvwprintw(tui->input_win, base_y, base_x, "%s", buffer);
                    wmove(tui->input_win, base_y, base_x + cursor_pos);
                    wrefresh(tui->input_win);
                }
                break;

            case KEY_LEFT:
                if (cursor_pos > 0) {
                    cursor_pos--;
                    wmove(tui->input_win, base_y, base_x + cursor_pos);
                    wrefresh(tui->input_win);
                }
                break;

            case KEY_RIGHT:
                if (cursor_pos < buf_len) {
                    cursor_pos++;
                    wmove(tui->input_win, base_y, base_x + cursor_pos);
                    wrefresh(tui->input_win);
                }
                break;

            case KEY_HOME:
            case 1: // Ctrl+A
                cursor_pos = 0;
                wmove(tui->input_win, base_y, base_x);
                wrefresh(tui->input_win);
                break;

            case KEY_END:
            case 5: // Ctrl+E
                cursor_pos = buf_len;
                wmove(tui->input_win, base_y, base_x + cursor_pos);
                wrefresh(tui->input_win);
                break;

            case 21: // Ctrl+U - clear line
                buf_len = 0;
                cursor_pos = 0;
                buffer[0] = '\0';
                werase(tui->input_win);
                box(tui->input_win, 0, 0);
                wattron(tui->input_win, COLOR_PAIR(COLOR_PAIR_PROMPT));
                mvwprintw(tui->input_win, base_y, 2, "%s", prompt);
                wattroff(tui->input_win, COLOR_PAIR(COLOR_PAIR_PROMPT));
                wmove(tui->input_win, base_y, base_x);
                wrefresh(tui->input_win);
                break;

            default:
                // Regular character input
                if (ch >= 32 && ch < 127 && buf_len < INPUT_BUFFER_SIZE - 1) {
                    // Check if we have space in the input area
                    if (cursor_pos < tui->screen_width - base_x - 3) {
                        // Insert character at cursor
                        memmove(&buffer[cursor_pos + 1], &buffer[cursor_pos], buf_len - cursor_pos + 1);
                        buffer[cursor_pos] = (char)ch;
                        buf_len++;
                        cursor_pos++;

                        // Redraw
                        werase(tui->input_win);
                        box(tui->input_win, 0, 0);
                        wattron(tui->input_win, COLOR_PAIR(COLOR_PAIR_PROMPT));
                        mvwprintw(tui->input_win, base_y, 2, "%s", prompt);
                        wattroff(tui->input_win, COLOR_PAIR(COLOR_PAIR_PROMPT));
                        mvwprintw(tui->input_win, base_y, base_x, "%s", buffer);
                        wmove(tui->input_win, base_y, base_x + cursor_pos);
                        wrefresh(tui->input_win);
                    }
                }
                break;
        }
    }

    // Clear input area after submission
    werase(tui->input_win);
    box(tui->input_win, 0, 0);
    wrefresh(tui->input_win);

    return buffer;
}

void tui_refresh(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    if (tui->conv_win) wrefresh(tui->conv_win);
    if (tui->status_win) wrefresh(tui->status_win);
    if (tui->input_win) wrefresh(tui->input_win);
}

void tui_clear_conversation(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    // Free all conversation lines
    for (int i = 0; i < tui->conv_lines_count; i++) {
        free(tui->conv_lines[i]);
    }
    tui->conv_lines_count = 0;
    tui->conv_scroll_offset = 0;

    // Clear window
    werase(tui->conv_win);
    wrefresh(tui->conv_win);
}

void tui_handle_resize(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    // Get new screen dimensions
    endwin();
    refresh();
    getmaxyx(stdscr, tui->screen_height, tui->screen_width);

    // Recalculate window heights
    tui->conv_height = tui->screen_height - tui->input_height - 1;

    // Recreate windows
    if (tui->conv_win) delwin(tui->conv_win);
    if (tui->status_win) delwin(tui->status_win);
    if (tui->input_win) delwin(tui->input_win);

    tui->conv_win = newwin(tui->conv_height, tui->screen_width, 0, 0);
    tui->status_win = newwin(1, tui->screen_width, tui->conv_height, 0);
    tui->input_win = newwin(tui->input_height, tui->screen_width,
                           tui->conv_height + 1, 0);

    scrollok(tui->conv_win, TRUE);
    idlok(tui->conv_win, TRUE);

    // Redraw everything
    tui_refresh(tui);
}
