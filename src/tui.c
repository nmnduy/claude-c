/*
 * TUI (Terminal User Interface) - ncurses-based implementation
 *
 * This module provides an ncurses-based input bar with full readline-like
 * keyboard shortcuts while preserving scrollback for conversation output.
 */

// Define feature test macros before any includes
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif

#include "tui.h"
#include "lineedit.h"
#define COLORSCHEME_EXTERN
#include "colorscheme.h"
#include "fallback_colors.h"
#include "indicators.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#include <ncurses.h>
#include <signal.h>
#include <sys/ioctl.h>

#define INITIAL_CONV_CAPACITY 1000
#define INPUT_BUFFER_SIZE 8192
#define INPUT_WIN_MIN_HEIGHT 3  // Min height for input window (1 line + 2 borders)
#define INPUT_WIN_MAX_HEIGHT 5  // Max height for input window (3 lines + 2 borders)

// Global spinner for TUI status updates
static Spinner *g_tui_spinner = NULL;

// Global flag to detect terminal resize
static volatile sig_atomic_t g_resize_flag = 0;

// Global TUI state pointer (for input resize callback)
static TUIState *g_tui_state_ptr = NULL;

// Ncurses color pair definitions (match TUIColorPair enum)
#define NCURSES_PAIR_FOREGROUND 1
#define NCURSES_PAIR_USER 2
#define NCURSES_PAIR_ASSISTANT 3
#define NCURSES_PAIR_STATUS 4
#define NCURSES_PAIR_ERROR 5
#define NCURSES_PAIR_PROMPT 6

// Signal handler for window resize
#ifdef SIGWINCH
static void handle_resize(int sig) {
    (void)sig;
    g_resize_flag = 1;
}
#endif

// Check if resize is pending and return the flag status
// This allows external code to check for resize events
int tui_resize_pending(void) {
    return g_resize_flag != 0;
}

// Convert RGB (0-255) to ncurses color (0-1000)
static short rgb_to_ncurses(int value) {
    return (short)((value * 1000) / 255);
}

// Initialize ncurses color pairs from our colorscheme
static void init_ncurses_colors(void) {
    // Check if terminal supports colors
    if (!has_colors()) {
        LOG_DEBUG("[TUI] Terminal does not support colors");
        return;
    }

    start_color();
    use_default_colors();  // Use terminal's default colors as base

    // If we have a loaded theme, use it to initialize custom colors
    if (g_theme_loaded) {
        LOG_DEBUG("[TUI] Initializing ncurses colors from loaded theme");

        // Define custom colors (colors 16-21 are safe to redefine)
        if (can_change_color()) {
            // Foreground
            init_color(16,
                rgb_to_ncurses(g_theme.foreground_rgb.r),
                rgb_to_ncurses(g_theme.foreground_rgb.g),
                rgb_to_ncurses(g_theme.foreground_rgb.b));

            // User (green)
            init_color(17,
                rgb_to_ncurses(g_theme.user_rgb.r),
                rgb_to_ncurses(g_theme.user_rgb.g),
                rgb_to_ncurses(g_theme.user_rgb.b));

            // Assistant (blue/cyan)
            init_color(18,
                rgb_to_ncurses(g_theme.assistant_rgb.r),
                rgb_to_ncurses(g_theme.assistant_rgb.g),
                rgb_to_ncurses(g_theme.assistant_rgb.b));

            // Status (yellow)
            init_color(19,
                rgb_to_ncurses(g_theme.status_rgb.r),
                rgb_to_ncurses(g_theme.status_rgb.g),
                rgb_to_ncurses(g_theme.status_rgb.b));

            // Error (red)
            init_color(20,
                rgb_to_ncurses(g_theme.error_rgb.r),
                rgb_to_ncurses(g_theme.error_rgb.g),
                rgb_to_ncurses(g_theme.error_rgb.b));

            // Initialize color pairs with custom colors
            init_pair(NCURSES_PAIR_FOREGROUND, 16, -1);  // -1 = default background
            init_pair(NCURSES_PAIR_USER, 17, -1);
            init_pair(NCURSES_PAIR_ASSISTANT, 18, -1);
            init_pair(NCURSES_PAIR_STATUS, 19, -1);
            init_pair(NCURSES_PAIR_ERROR, 20, -1);
            init_pair(NCURSES_PAIR_PROMPT, 17, -1);  // Use USER color for prompt

            LOG_DEBUG("[TUI] Custom colors initialized successfully");
        } else {
            LOG_DEBUG("[TUI] Terminal does not support color changes, using standard colors");
            // Fall back to standard ncurses colors
            init_pair(NCURSES_PAIR_FOREGROUND, COLOR_WHITE, -1);
            init_pair(NCURSES_PAIR_USER, COLOR_GREEN, -1);
            init_pair(NCURSES_PAIR_ASSISTANT, COLOR_CYAN, -1);
            init_pair(NCURSES_PAIR_STATUS, COLOR_YELLOW, -1);
            init_pair(NCURSES_PAIR_ERROR, COLOR_RED, -1);
            init_pair(NCURSES_PAIR_PROMPT, COLOR_GREEN, -1);
        }
    } else {
        LOG_DEBUG("[TUI] No theme loaded, using standard ncurses colors");
        // Use standard ncurses color constants
        init_pair(NCURSES_PAIR_FOREGROUND, COLOR_WHITE, -1);
        init_pair(NCURSES_PAIR_USER, COLOR_GREEN, -1);
        init_pair(NCURSES_PAIR_ASSISTANT, COLOR_CYAN, -1);
        init_pair(NCURSES_PAIR_STATUS, COLOR_YELLOW, -1);
        init_pair(NCURSES_PAIR_ERROR, COLOR_RED, -1);
        init_pair(NCURSES_PAIR_PROMPT, COLOR_GREEN, -1);
    }
}

// Clear the resize flag (called after handling resize)
void tui_clear_resize_flag(void) {
    g_resize_flag = 0;
}

// UTF-8 helper functions (from lineedit.c)
static int utf8_char_length(unsigned char first_byte) {
    if ((first_byte & 0x80) == 0) return 1;  // 0xxxxxxx
    if ((first_byte & 0xE0) == 0xC0) return 2;  // 110xxxxx
    if ((first_byte & 0xF0) == 0xE0) return 3;  // 1110xxxx
    if ((first_byte & 0xF8) == 0xF0) return 4;  // 11110xxx
    return 1;  // Invalid, treat as single byte
}

// Not used in current implementation, but kept for potential future UTF-8 handling
// static int is_utf8_continuation(unsigned char byte) {
//     return (byte & 0xC0) == 0x80;
// }

static int is_word_boundary(char c) {
    return !isalnum(c) && c != '_';
}

static int move_backward_word(const char *buffer, int cursor_pos) {
    if (cursor_pos <= 0) return 0;
    int pos = cursor_pos - 1;
    while (pos > 0 && is_word_boundary(buffer[pos])) pos--;
    while (pos > 0 && !is_word_boundary(buffer[pos])) pos--;
    if (pos > 0 && is_word_boundary(buffer[pos])) pos++;
    return pos;
}

static int move_forward_word(const char *buffer, int cursor_pos, int buffer_len) {
    if (cursor_pos >= buffer_len) return buffer_len;
    int pos = cursor_pos;
    while (pos < buffer_len && !is_word_boundary(buffer[pos])) pos++;
    while (pos < buffer_len && is_word_boundary(buffer[pos])) pos++;
    return pos;
}

// Input buffer management
typedef struct {
    char *buffer;
    size_t capacity;
    int length;
    int cursor;
    WINDOW *win;
    int win_width;
    int win_height;
    // Display state
    int view_offset;  // Horizontal scroll offset for long lines
    int line_scroll_offset;  // Vertical scroll offset (which line to show at top)
} InputState;

static InputState g_input_state = {0};

// Calculate how many visual lines are needed for the current buffer
// Note: This assumes first line includes the prompt
static int calculate_needed_lines(const char *buffer, int buffer_len, int available_width, int prompt_len) {
    if (buffer_len == 0) return 1;

    int lines = 1;
    int current_col = prompt_len;  // First line starts after prompt
    int current_line = 0;

    for (int i = 0; i < buffer_len; i++) {
        if (buffer[i] == '\n') {
            lines++;
            current_line++;
            current_col = 0;  // Newlines don't have prompt
        } else {
            current_col++;
            // First line has prompt, others don't
            int line_width = (current_line == 0) ? available_width + prompt_len : available_width;
            if (current_col >= line_width) {
                lines++;
                current_line++;
                current_col = 0;
            }
        }
    }

    return lines;
}

// Resize input window dynamically (called from redraw)
static int resize_input_window(TUIState *tui, int desired_lines) {
    if (!tui || !tui->is_initialized) return -1;

    // Clamp to min/max (1-3 lines of content, +2 for borders)
    int min_window_height = INPUT_WIN_MIN_HEIGHT;  // 1 line + borders
    int max_window_height = INPUT_WIN_MAX_HEIGHT;  // 3 lines + borders
    int new_window_height = desired_lines + 2;  // +2 for borders

    if (new_window_height < min_window_height) {
        new_window_height = min_window_height;
    } else if (new_window_height > max_window_height) {
        new_window_height = max_window_height;
    }

    // Only resize if height actually changed
    if (new_window_height == tui->input_height) {
        return 0;
    }

    // Delete old window
    if (tui->input_win) {
        delwin(tui->input_win);
    }

    // Create new window with adjusted height
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    tui->input_height = new_window_height;
    tui->input_win = newwin(new_window_height, max_x, max_y - new_window_height, 0);

    if (!tui->input_win) {
        LOG_ERROR("Failed to resize input window");
        return -1;
    }

    // Update input state
    g_input_state.win = tui->input_win;
    int h, w;
    getmaxyx(tui->input_win, h, w);
    g_input_state.win_width = w - 2;
    g_input_state.win_height = h - 2;

    // Enable keypad for new window
    keypad(tui->input_win, TRUE);

    // Refresh stdscr to clear old area
    touchwin(stdscr);
    refresh();

    return 0;
}

// Initialize input buffer
static int input_init(WINDOW *win) {
    g_input_state.buffer = malloc(INPUT_BUFFER_SIZE);
    if (!g_input_state.buffer) {
        return -1;
    }
    g_input_state.capacity = INPUT_BUFFER_SIZE;
    g_input_state.buffer[0] = '\0';
    g_input_state.length = 0;
    g_input_state.cursor = 0;
    g_input_state.win = win;
    g_input_state.view_offset = 0;
    g_input_state.line_scroll_offset = 0;

    // Get window dimensions
    int h, w;
    getmaxyx(win, h, w);
    g_input_state.win_width = w - 2;  // Account for borders
    g_input_state.win_height = h - 2;

    return 0;
}

// Free input buffer
static void input_free(void) {
    free(g_input_state.buffer);
    g_input_state.buffer = NULL;
    g_input_state.capacity = 0;
    g_input_state.length = 0;
    g_input_state.cursor = 0;
}

// Insert character(s) at cursor position
static int input_insert_char(const unsigned char *utf8_char, int char_bytes) {
    if (g_input_state.length + char_bytes >= (int)g_input_state.capacity - 1) {
        return -1;  // Buffer full
    }

    // Make space for the new character(s)
    memmove(&g_input_state.buffer[g_input_state.cursor + char_bytes],
            &g_input_state.buffer[g_input_state.cursor],
            (size_t)(g_input_state.length - g_input_state.cursor + 1));

    // Copy the character bytes
    for (int i = 0; i < char_bytes; i++) {
        g_input_state.buffer[g_input_state.cursor + i] = (char)utf8_char[i];
    }

    g_input_state.length += char_bytes;
    g_input_state.cursor += char_bytes;
    return 0;
}

// Delete character at cursor position (forward delete)
static int input_delete_char(void) {
    if (g_input_state.cursor >= g_input_state.length) {
        return 0;  // Nothing to delete
    }

    // Find the length of the UTF-8 character at cursor
    int char_len = utf8_char_length((unsigned char)g_input_state.buffer[g_input_state.cursor]);

    // Delete the character by moving subsequent text left
    memmove(&g_input_state.buffer[g_input_state.cursor],
            &g_input_state.buffer[g_input_state.cursor + char_len],
            (size_t)(g_input_state.length - g_input_state.cursor - char_len + 1));

    g_input_state.length -= char_len;
    return char_len;
}

// Delete character before cursor (backspace)
static int input_backspace(void) {
    if (g_input_state.cursor <= 0) {
        return 0;  // Nothing to delete
    }

    memmove(&g_input_state.buffer[g_input_state.cursor - 1],
            &g_input_state.buffer[g_input_state.cursor],
            (size_t)(g_input_state.length - g_input_state.cursor + 1));
    g_input_state.length--;
    g_input_state.cursor--;
    return 1;
}

// Delete word before cursor (Alt+Backspace)
static int input_delete_word_backward(void) {
    if (g_input_state.cursor <= 0) {
        return 0;
    }

    int word_start = g_input_state.cursor - 1;
    while (word_start > 0 && is_word_boundary(g_input_state.buffer[word_start])) {
        word_start--;
    }
    while (word_start > 0 && !is_word_boundary(g_input_state.buffer[word_start])) {
        word_start--;
    }
    if (word_start > 0 && is_word_boundary(g_input_state.buffer[word_start])) {
        word_start++;
    }

    int delete_count = g_input_state.cursor - word_start;
    if (delete_count > 0) {
        memmove(&g_input_state.buffer[word_start],
                &g_input_state.buffer[g_input_state.cursor],
                (size_t)(g_input_state.length - g_input_state.cursor + 1));
        g_input_state.length -= delete_count;
        g_input_state.cursor = word_start;
    }

    return delete_count;
}

// Delete word forward (Alt+d)
static int input_delete_word_forward(void) {
    if (g_input_state.cursor >= g_input_state.length) {
        return 0;
    }

    int word_end = move_forward_word(g_input_state.buffer, g_input_state.cursor, g_input_state.length);
    int delete_count = word_end - g_input_state.cursor;

    if (delete_count > 0) {
        memmove(&g_input_state.buffer[g_input_state.cursor],
                &g_input_state.buffer[word_end],
                (size_t)(g_input_state.length - word_end + 1));
        g_input_state.length -= delete_count;
    }

    return delete_count;
}

// Redraw the input window
static void input_redraw(const char *prompt) {
    WINDOW *win = g_input_state.win;
    if (!win) return;

    int prompt_len = (int)strlen(prompt) + 1;  // +1 for space after prompt

    // Calculate available width for text (window width - borders - prompt)
    int available_width = g_input_state.win_width - prompt_len;
    if (available_width < 10) available_width = 10;

    // Calculate how many lines we need to display all content
    int needed_lines = calculate_needed_lines(g_input_state.buffer, g_input_state.length, available_width, prompt_len);

    // Request window resize (this will be a no-op if size hasn't changed)
    if (g_tui_state_ptr) {
        resize_input_window(g_tui_state_ptr, needed_lines);
        win = g_input_state.win;  // Window may have been recreated
        if (!win) return;
    }

    // Calculate cursor line position
    int cursor_line = 0;
    int cursor_col = prompt_len;
    for (int i = 0; i < g_input_state.cursor; i++) {
        if (g_input_state.buffer[i] == '\n') {
            cursor_line++;
            cursor_col = 0;
        } else {
            cursor_col++;
            if (cursor_col >= available_width + prompt_len) {
                cursor_line++;
                cursor_col = 0;
            }
        }
    }

    // Adjust vertical scroll to keep cursor visible
    int max_visible_lines = g_input_state.win_height;
    if (cursor_line < g_input_state.line_scroll_offset) {
        g_input_state.line_scroll_offset = cursor_line;
    } else if (cursor_line >= g_input_state.line_scroll_offset + max_visible_lines) {
        g_input_state.line_scroll_offset = cursor_line - max_visible_lines + 1;
    }

    // Clear the window
    werase(win);

    // Draw box with accent color if colors are available
    if (has_colors()) {
        wattron(win, COLOR_PAIR(NCURSES_PAIR_ASSISTANT));
        box(win, 0, 0);
        wattroff(win, COLOR_PAIR(NCURSES_PAIR_ASSISTANT));
    } else {
        box(win, 0, 0);
    }

    // Draw prompt on first visible line (if we're not scrolled past it)
    if (g_input_state.line_scroll_offset == 0) {
        if (has_colors()) {
            wattron(win, COLOR_PAIR(NCURSES_PAIR_PROMPT) | A_BOLD);
            mvwprintw(win, 1, 1, "%s ", prompt);
            wattroff(win, COLOR_PAIR(NCURSES_PAIR_PROMPT) | A_BOLD);
        } else {
            mvwprintw(win, 1, 1, "%s ", prompt);
        }
    }

    // Render visible lines with scrolling support
    if (has_colors()) {
        wattron(win, COLOR_PAIR(NCURSES_PAIR_FOREGROUND));
    }

    int current_line = 0;
    int screen_y = 1;
    int screen_x = (current_line == 0) ? prompt_len + 1 : 1;

    for (int i = 0; i < g_input_state.length && screen_y <= g_input_state.win_height; i++) {
        // Skip lines before scroll offset
        if (current_line < g_input_state.line_scroll_offset) {
            if (g_input_state.buffer[i] == '\n') {
                current_line++;
                screen_x = 0;
            } else {
                screen_x++;
                if (screen_x >= available_width + ((current_line == 0) ? prompt_len : 0)) {
                    current_line++;
                    screen_x = 0;
                }
            }
            continue;
        }

        // Render character
        char c = g_input_state.buffer[i];
        if (c == '\n') {
            // Show newline as dimmed character (using + as fallback for arrow)
            mvwaddch(win, screen_y, screen_x, (unsigned char)'+' | A_DIM);
            screen_y++;
            current_line++;
            screen_x = 1;  // Reset to left edge (after border)
        } else {
            mvwaddch(win, screen_y, screen_x, c);
            screen_x++;

            // Check if we need to wrap
            int line_width = available_width;
            if (current_line == g_input_state.line_scroll_offset && g_input_state.line_scroll_offset == 0) {
                line_width += prompt_len;  // First line includes prompt
            }

            if (screen_x > line_width) {
                screen_y++;
                current_line++;
                screen_x = 1;
            }
        }
    }

    if (has_colors()) {
        wattroff(win, COLOR_PAIR(NCURSES_PAIR_FOREGROUND));
    }

    // Position cursor (adjusted for scroll)
    int cursor_screen_y = cursor_line - g_input_state.line_scroll_offset + 1;
    int cursor_screen_x = cursor_col + 1;  // +1 for border

    // Recalculate cursor_col relative to its line
    int temp_line = 0;
    int temp_col = (temp_line == 0) ? prompt_len : 0;
    for (int i = 0; i < g_input_state.cursor; i++) {
        if (g_input_state.buffer[i] == '\n') {
            temp_line++;
            temp_col = 0;
        } else {
            temp_col++;
            int line_width = available_width + ((temp_line == 0) ? prompt_len : 0);
            if (temp_col >= line_width) {
                temp_line++;
                temp_col = 0;
            }
        }
    }
    cursor_screen_x = temp_col + 1;

    // Bounds check for cursor position
    if (cursor_screen_y >= 1 && cursor_screen_y <= g_input_state.win_height &&
        cursor_screen_x >= 1 && cursor_screen_x <= g_input_state.win_width) {
        wmove(win, cursor_screen_y, cursor_screen_x);
    }

    wrefresh(win);
}

int tui_init(TUIState *tui) {
    if (!tui) return -1;

    // Store global pointer for input resize callback
    g_tui_state_ptr = tui;

    // Set locale for UTF-8 support
    setlocale(LC_ALL, "");

    // Initialize ncurses
    initscr();
    cbreak();  // Disable line buffering
    noecho();  // Don't echo input
    keypad(stdscr, TRUE);  // Enable function keys
    nodelay(stdscr, FALSE);  // Blocking input
    curs_set(2);  // Make cursor very visible (block cursor)

    // Initialize colors from colorscheme
    init_ncurses_colors();

    // Create input window at bottom of screen
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    tui->screen_height = max_y;
    tui->screen_width = max_x;
    tui->input_height = INPUT_WIN_MIN_HEIGHT;  // Start with minimum height

    // Create input window (start with minimum height)
    tui->input_win = newwin(INPUT_WIN_MIN_HEIGHT, max_x, max_y - INPUT_WIN_MIN_HEIGHT, 0);
    if (!tui->input_win) {
        endwin();
        return -1;
    }

    // Initialize input buffer
    if (input_init(tui->input_win) != 0) {
        delwin(tui->input_win);
        endwin();
        return -1;
    }

    // Register resize handler (if available)
#ifdef SIGWINCH
    signal(SIGWINCH, handle_resize);
#endif

    tui->is_initialized = 1;
    refresh();

    return 0;
}

void tui_cleanup(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    // Clear global pointer
    g_tui_state_ptr = NULL;

    // Stop any running spinner
    if (g_tui_spinner) {
        spinner_stop(g_tui_spinner, NULL, 1);
        g_tui_spinner = NULL;
    }

    // Free input state
    input_free();

    // Destroy windows
    if (tui->input_win) {
        delwin(tui->input_win);
        tui->input_win = NULL;
    }

    // End ncurses
    endwin();

    tui->is_initialized = 0;

    // Print a newline to ensure clean exit
    printf("\n");
    fflush(stdout);
}

void tui_add_conversation_line(TUIState *tui, const char *prefix, const char *text, TUIColorPair color_pair) {
    if (!tui || !tui->is_initialized) return;

    // Stop any running spinner before printing conversation lines
    if (g_tui_spinner) {
        spinner_stop(g_tui_spinner, NULL, 1);
        g_tui_spinner = NULL;
    }

    // Move to main screen (above input window)
    // We'll just print to stdout, letting it scroll naturally
    // This preserves terminal scrollback

    // Get colors from colorscheme or fall back to centralized defaults
    const char *prefix_color_start = "";
    const char *text_color_start = "";
    const char *color_end = ANSI_RESET;
    char prefix_color_code[32];
    char text_color_code[32];

    // Use foreground color for main text, accent colors only for role names/prefixes
    switch (color_pair) {
        case COLOR_PAIR_DEFAULT:
        case COLOR_PAIR_FOREGROUND:
            if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
                text_color_start = text_color_code;
            } else {
                text_color_start = ANSI_FALLBACK_FOREGROUND;
            }
            prefix_color_start = text_color_start;
            break;

        case COLOR_PAIR_USER:
            if (get_colorscheme_color(COLORSCHEME_USER, prefix_color_code, sizeof(prefix_color_code)) == 0) {
                prefix_color_start = prefix_color_code;
            } else {
                prefix_color_start = ANSI_FALLBACK_USER;
            }
            if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
                text_color_start = text_color_code;
            } else {
                text_color_start = ANSI_FALLBACK_FOREGROUND;
            }
            break;

        case COLOR_PAIR_ASSISTANT:
            if (get_colorscheme_color(COLORSCHEME_ASSISTANT, prefix_color_code, sizeof(prefix_color_code)) == 0) {
                prefix_color_start = prefix_color_code;
            } else {
                prefix_color_start = ANSI_FALLBACK_ASSISTANT;
            }
            if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
                text_color_start = text_color_code;
            } else {
                text_color_start = ANSI_FALLBACK_FOREGROUND;
            }
            break;

        case COLOR_PAIR_TOOL:
            if (get_colorscheme_color(COLORSCHEME_TOOL, prefix_color_code, sizeof(prefix_color_code)) == 0) {
                prefix_color_start = prefix_color_code;
            } else {
                prefix_color_start = ANSI_FALLBACK_TOOL;
            }
            if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
                text_color_start = text_color_code;
            } else {
                text_color_start = ANSI_FALLBACK_FOREGROUND;
            }
            break;

        case COLOR_PAIR_ERROR:
            if (get_colorscheme_color(COLORSCHEME_ERROR, prefix_color_code, sizeof(prefix_color_code)) == 0) {
                prefix_color_start = prefix_color_code;
            } else {
                prefix_color_start = ANSI_FALLBACK_ERROR;
            }
            if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
                text_color_start = text_color_code;
            } else {
                text_color_start = ANSI_FALLBACK_FOREGROUND;
            }
            break;

        case COLOR_PAIR_STATUS:
            if (get_colorscheme_color(COLORSCHEME_STATUS, prefix_color_code, sizeof(prefix_color_code)) == 0) {
                prefix_color_start = prefix_color_code;
            } else {
                prefix_color_start = ANSI_FALLBACK_STATUS;
            }
            if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
                text_color_start = text_color_code;
            } else {
                text_color_start = ANSI_FALLBACK_FOREGROUND;
            }
            break;

        case COLOR_PAIR_PROMPT:
            if (get_colorscheme_color(COLORSCHEME_USER, prefix_color_code, sizeof(prefix_color_code)) == 0) {
                prefix_color_start = prefix_color_code;
            } else {
                prefix_color_start = ANSI_FALLBACK_USER;
            }
            if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
                text_color_start = text_color_code;
            } else {
                text_color_start = ANSI_FALLBACK_FOREGROUND;
            }
            break;

        default:
            prefix_color_start = "";
            text_color_start = "";
            color_end = "";
            break;
    }

    // Print with separate colors for prefix and text (to stdout, above ncurses window)
    if (prefix) {
        printf("%s%s%s %s%s%s\n", prefix_color_start, prefix, color_end, text_color_start, text, color_end);
    } else {
        printf("%s%s%s\n", text_color_start, text, color_end);
    }
    fflush(stdout);

    // Redraw input window to ensure it stays visible
    if (tui->input_win) {
        touchwin(tui->input_win);
        wrefresh(tui->input_win);
    }
}

void tui_update_status(TUIState *tui, const char *status_text) {
    if (!tui || !tui->is_initialized) return;

    // If status text is empty, stop any running spinner
    if (!status_text || strlen(status_text) == 0) {
        if (g_tui_spinner) {
            spinner_stop(g_tui_spinner, NULL, 1);
            g_tui_spinner = NULL;
        }
        return;
    }

    // Update spinner or start new one
    if (g_tui_spinner) {
        spinner_update(g_tui_spinner, status_text);
    } else {
        g_tui_spinner = spinner_start(status_text, SPINNER_CYAN);
    }
}

char* tui_read_input(TUIState *tui, const char *prompt) {
    if (!tui || !tui->is_initialized || !tui->input_win) return NULL;

    // Stop any running spinner before showing input prompt
    if (g_tui_spinner) {
        spinner_stop(g_tui_spinner, NULL, 1);
        g_tui_spinner = NULL;
    }

    // Reset input state
    g_input_state.buffer[0] = '\0';
    g_input_state.length = 0;
    g_input_state.cursor = 0;
    g_input_state.view_offset = 0;

    // Initial draw
    input_redraw(prompt);

    int running = 1;
    while (running) {
        // Check for resize
        if (g_resize_flag) {
            g_resize_flag = 0;
            tui_handle_resize(tui);
            input_redraw(prompt);
            continue;
        }

        int ch = wgetch(tui->input_win);

        // Handle special keys
        if (ch == KEY_RESIZE) {
            tui_handle_resize(tui);
            input_redraw(prompt);
            continue;
        } else if (ch == 1) {  // Ctrl+A: beginning of line
            g_input_state.cursor = 0;
            input_redraw(prompt);
        } else if (ch == 5) {  // Ctrl+E: end of line
            g_input_state.cursor = g_input_state.length;
            input_redraw(prompt);
        } else if (ch == 4) {  // Ctrl+D: EOF
            return NULL;
        } else if (ch == 11) {  // Ctrl+K: kill to end of line
            g_input_state.buffer[g_input_state.cursor] = '\0';
            g_input_state.length = g_input_state.cursor;
            input_redraw(prompt);
        } else if (ch == 21) {  // Ctrl+U: kill to beginning of line
            if (g_input_state.cursor > 0) {
                memmove(g_input_state.buffer,
                       &g_input_state.buffer[g_input_state.cursor],
                       (size_t)(g_input_state.length - g_input_state.cursor + 1));
                g_input_state.length -= g_input_state.cursor;
                g_input_state.cursor = 0;
                input_redraw(prompt);
            }
        } else if (ch == 12) {  // Ctrl+L: clear input
            g_input_state.buffer[0] = '\0';
            g_input_state.length = 0;
            g_input_state.cursor = 0;
            input_redraw(prompt);
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {  // Backspace
            if (input_backspace() > 0) {
                input_redraw(prompt);
            }
        } else if (ch == KEY_DC) {  // Delete key
            if (input_delete_char() > 0) {
                input_redraw(prompt);
            }
        } else if (ch == KEY_LEFT) {  // Left arrow
            if (g_input_state.cursor > 0) {
                g_input_state.cursor--;
                input_redraw(prompt);
            }
        } else if (ch == KEY_RIGHT) {  // Right arrow
            if (g_input_state.cursor < g_input_state.length) {
                g_input_state.cursor++;
                input_redraw(prompt);
            }
        } else if (ch == KEY_HOME) {  // Home
            g_input_state.cursor = 0;
            input_redraw(prompt);
        } else if (ch == KEY_END) {  // End
            g_input_state.cursor = g_input_state.length;
            input_redraw(prompt);
        } else if (ch == 10) {  // Ctrl+J: insert newline
            unsigned char newline = '\n';
            if (input_insert_char(&newline, 1) == 0) {
                input_redraw(prompt);
            }
        } else if (ch == 13) {  // Enter: submit
            running = 0;
        } else if (ch == 27) {  // ESC sequence (Alt key combinations)
            // Set nodelay to check for following character
            nodelay(tui->input_win, TRUE);
            int next_ch = wgetch(tui->input_win);
            nodelay(tui->input_win, FALSE);

            if (next_ch == ERR) {
                // Standalone ESC, ignore
                continue;
            } else if (next_ch == 'b' || next_ch == 'B') {  // Alt+b: backward word
                g_input_state.cursor = move_backward_word(g_input_state.buffer, g_input_state.cursor);
                input_redraw(prompt);
            } else if (next_ch == 'f' || next_ch == 'F') {  // Alt+f: forward word
                g_input_state.cursor = move_forward_word(g_input_state.buffer, g_input_state.cursor, g_input_state.length);
                input_redraw(prompt);
            } else if (next_ch == 'd' || next_ch == 'D') {  // Alt+d: delete next word
                if (input_delete_word_forward() > 0) {
                    input_redraw(prompt);
                }
            } else if (next_ch == KEY_BACKSPACE || next_ch == 127 || next_ch == 8) {  // Alt+Backspace: delete previous word
                if (input_delete_word_backward() > 0) {
                    input_redraw(prompt);
                }
            }
        } else if (ch >= 32 && ch < 127) {  // Printable ASCII
            unsigned char c = (unsigned char)ch;
            if (input_insert_char(&c, 1) == 0) {
                input_redraw(prompt);
            }
        } else if (ch >= 128) {  // UTF-8 multibyte character (basic support)
            // ncurses handles most UTF-8 automatically, just insert as-is
            unsigned char c = (unsigned char)ch;
            if (input_insert_char(&c, 1) == 0) {
                input_redraw(prompt);
            }
        }
    }

    // Return a copy of the buffer
    return strdup(g_input_state.buffer);
}

void tui_refresh(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    if (tui->input_win) {
        touchwin(tui->input_win);
        wrefresh(tui->input_win);
    }
    refresh();
}

void tui_clear_conversation(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    // Get accent color for status indicator
    char status_color_code[32];
    char text_color_code[32];
    const char *status_color_start;
    const char *text_color_start;

    if (get_colorscheme_color(COLORSCHEME_STATUS, status_color_code, sizeof(status_color_code)) == 0) {
        status_color_start = status_color_code;
    } else {
        status_color_start = ANSI_FALLBACK_STATUS;
    }

    if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
        text_color_start = text_color_code;
    } else {
        text_color_start = ANSI_FALLBACK_FOREGROUND;
    }

    printf("%s[System]%s %sConversation history cleared (kept in terminal scrollback)%s\n",
           status_color_start, ANSI_RESET, text_color_start, ANSI_RESET);
    fflush(stdout);
}

void tui_handle_resize(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    // Properly handle ncurses resize
    // This sequence is important for correct resize handling
    endwin();         // End the current ncurses session
    refresh();        // Refresh stdscr to get new dimensions
    clear();          // Clear the screen

    // Update screen dimensions
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    tui->screen_height = max_y;
    tui->screen_width = max_x;

    // Resize and reposition input window
    if (tui->input_win) {
        // Delete and recreate window to avoid ncurses resize issues
        delwin(tui->input_win);
        tui->input_win = newwin(tui->input_height, max_x, max_y - tui->input_height, 0);

        if (tui->input_win) {
            // Re-enable keypad for the new window
            keypad(tui->input_win, TRUE);

            // Update input state dimensions
            int h, w;
            getmaxyx(tui->input_win, h, w);
            g_input_state.win_width = w - 2;
            g_input_state.win_height = h - 2;
            g_input_state.win = tui->input_win;
        }
    }

    refresh();
}

void tui_show_startup_banner(TUIState *tui, const char *version, const char *model, const char *working_dir) {
    if (!tui || !tui->is_initialized) return;

    // Get accent color for the Claude mascot (assistant color)
    char mascot_color_code[32];
    char text_color_code[32];
    const char *mascot_color_start;
    const char *text_color_start;

    if (get_colorscheme_color(COLORSCHEME_ASSISTANT, mascot_color_code, sizeof(mascot_color_code)) == 0) {
        mascot_color_start = mascot_color_code;
    } else {
        mascot_color_start = ANSI_FALLBACK_BOLD_CYAN;
    }

    if (get_colorscheme_color(COLORSCHEME_FOREGROUND, text_color_code, sizeof(text_color_code)) == 0) {
        text_color_start = text_color_code;
    } else {
        text_color_start = ANSI_FALLBACK_FOREGROUND;
    }

    // Print banner (to stdout, above ncurses window)
    printf("%s", mascot_color_start);
    printf(" ▐▛███▜▌");
    printf("%s   claude-c v%s\n", text_color_start, version);
    printf("%s▝▜█████▛▘%s  %s\n", mascot_color_start, text_color_start, model);
    printf("%s  ▘▘ ▝▝%s    %s\n", mascot_color_start, text_color_start, working_dir);
    printf(ANSI_RESET "\n");
    fflush(stdout);
}

void tui_render_todo_list(TUIState *tui, const TodoList *todo_list) {
    if (!tui || !tui->is_initialized || !todo_list) return;

    // Stop any running spinner before rendering TODO list
    if (g_tui_spinner) {
        spinner_stop(g_tui_spinner, NULL, 1);
        g_tui_spinner = NULL;
    }

    // Call todo_render function which handles the formatting
    todo_render(todo_list);
}
