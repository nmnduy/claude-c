/*
 * TUI (Terminal User Interface) - Simple terminal interface
 */

#include "tui.h"
#include "lineedit.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

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

    // Set locale for UTF-8 support
    setlocale(LC_ALL, "");

    // Simple initialization - we'll use raw terminal mode instead of full ncurses
    // This avoids the alternate screen buffer issue

    // Get terminal size using TIOCGWINSZ
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        tui->screen_height = w.ws_row;
        tui->screen_width = w.ws_col;
    } else {
        tui->screen_height = 24;  // Default
        tui->screen_width = 80;
    }

    tui->input_height = 3;

    // We don't actually need ncurses windows - just mark as initialized
    tui->conv_win = NULL;
    tui->status_win = NULL;
    tui->input_win = NULL;
    tui->conv_lines = NULL;
    tui->conv_lines_count = 0;
    tui->conv_lines_capacity = 0;
    tui->conv_scroll_offset = 0;

    tui->is_initialized = 1;

    return 0;
}

void tui_cleanup(TUIState *tui) {
    if (!tui) return;

    tui->is_initialized = 0;

    // Print a newline to ensure clean exit
    printf("\n");
    fflush(stdout);
}

void tui_add_conversation_line(TUIState *tui, const char *prefix, const char *text, TUIColorPair color_pair) {
    if (!tui || !tui->is_initialized) return;

    // Map color pairs to ANSI escape codes
    const char *color_start = "";
    const char *color_end = "\033[0m";

    switch (color_pair) {
        case COLOR_PAIR_USER:
            color_start = "\033[32m";  // Green
            break;
        case COLOR_PAIR_ASSISTANT:
            color_start = "\033[34m";  // Blue
            break;
        case COLOR_PAIR_TOOL:
            color_start = "\033[33m";  // Yellow
            break;
        case COLOR_PAIR_ERROR:
            color_start = "\033[31m";  // Red
            break;
        case COLOR_PAIR_STATUS:
            color_start = "\033[36m";  // Cyan
            break;
        default:
            color_start = "";
            color_end = "";
            break;
    }

    // Print directly to stdout with color
    if (prefix) {
        printf("%s%s %s%s\n", color_start, prefix, text, color_end);
    } else {
        printf("%s%s%s\n", color_start, text, color_end);
    }
    fflush(stdout);
}

void tui_update_status(TUIState *tui, const char *status_text) {
    if (!tui || !tui->is_initialized) return;

    // Just print status to stdout with cyan color
    printf("\033[36m[Status] %s\033[0m\n", status_text);
    fflush(stdout);
}

char* tui_read_input(TUIState *tui, const char *prompt) {
    if (!tui || !tui->is_initialized) return NULL;

    // Use lineedit for input handling - simple and works with terminal scrolling
    LineEditor editor;
    lineedit_init(&editor, NULL, NULL);  // No completion for now

    // Create colored prompt
    char colored_prompt[256];
    snprintf(colored_prompt, sizeof(colored_prompt), "\033[32m%s\033[0m ", prompt);

    // Read input
    char *input = lineedit_readline(&editor, colored_prompt);

    lineedit_free(&editor);

    return input;
}

void tui_refresh(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    // Nothing to refresh - using simple terminal output
}

void tui_clear_conversation(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    // Just print a message - conversation is in terminal scrollback
    printf("\033[36m[System] Conversation history cleared (kept in terminal scrollback)\033[0m\n");
    fflush(stdout);
}

void tui_handle_resize(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    // Update screen dimensions
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        tui->screen_height = w.ws_row;
        tui->screen_width = w.ws_col;
    }
}

void tui_show_startup_banner(TUIState *tui, const char *version, const char *model, const char *working_dir) {
    if (!tui || !tui->is_initialized) return;

    // Print banner directly to stdout with blue/bold color
    printf("\033[1;34m");  // Bold blue
    printf(" ▐▛███▜▌   claude-c v%s\n", version);
    printf("▝▜█████▛▘  %s\n", model);
    printf("  ▘▘ ▝▝    %s\n", working_dir);
    printf("\033[0m\n");  // Reset color and add blank line
    fflush(stdout);
}
