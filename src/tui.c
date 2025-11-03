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
#define INPUT_WIN_HEIGHT 5  // Height for input window (includes borders)

// Global spinner for TUI status updates
static Spinner *g_tui_spinner = NULL;

// Global flag to detect terminal resize
static volatile sig_atomic_t g_resize_flag = 0;

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
} InputState;

static InputState g_input_state = {0};

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
    
    // Clear the window
    werase(win);
    box(win, 0, 0);
    
    // Draw prompt in top-left corner (inside border)
    mvwprintw(win, 1, 1, "%s", prompt);
    int prompt_len = (int)strlen(prompt);
    
    // Calculate visible portion of buffer
    int available_width = g_input_state.win_width - prompt_len - 1;
    if (available_width < 10) available_width = 10;  // Minimum visible width
    
    // Adjust view offset to keep cursor visible
    if (g_input_state.cursor < g_input_state.view_offset) {
        g_input_state.view_offset = g_input_state.cursor;
    }
    if (g_input_state.cursor > g_input_state.view_offset + available_width) {
        g_input_state.view_offset = g_input_state.cursor - available_width;
    }
    
    // Draw visible portion of buffer
    int visible_start = g_input_state.view_offset;
    int visible_end = g_input_state.view_offset + available_width;
    if (visible_end > g_input_state.length) {
        visible_end = g_input_state.length;
    }
    
    // Handle multiline display (wrap at window width)
    int y = 1;
    int x = prompt_len + 1;
    for (int i = visible_start; i < visible_end && y < g_input_state.win_height; i++) {
        char c = g_input_state.buffer[i];
        if (c == '\n') {
            // Manual newline
            y++;
            x = 1;
            if (y >= g_input_state.win_height) break;
        } else {
            // Regular character
            mvwaddch(win, y, x, c);
            x++;
            // Wrap at window edge
            if (x >= g_input_state.win_width) {
                y++;
                x = 1;
                if (y >= g_input_state.win_height) break;
            }
        }
    }
    
    // Position cursor
    // Calculate cursor position in window coordinates
    int cursor_y = 1;
    int cursor_x = prompt_len + 1;
    for (int i = visible_start; i < g_input_state.cursor && i < visible_end; i++) {
        if (g_input_state.buffer[i] == '\n') {
            cursor_y++;
            cursor_x = 1;
        } else {
            cursor_x++;
            if (cursor_x >= g_input_state.win_width) {
                cursor_y++;
                cursor_x = 1;
            }
        }
    }
    
    wmove(win, cursor_y, cursor_x);
    wrefresh(win);
}

int tui_init(TUIState *tui) {
    if (!tui) return -1;
    
    // Set locale for UTF-8 support
    setlocale(LC_ALL, "");
    
    // Initialize ncurses
    initscr();
    cbreak();  // Disable line buffering
    noecho();  // Don't echo input
    keypad(stdscr, TRUE);  // Enable function keys
    nodelay(stdscr, FALSE);  // Blocking input
    
    // Create input window at bottom of screen
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    tui->screen_height = max_y;
    tui->screen_width = max_x;
    tui->input_height = INPUT_WIN_HEIGHT;
    
    // Create input window
    tui->input_win = newwin(INPUT_WIN_HEIGHT, max_x, max_y - INPUT_WIN_HEIGHT, 0);
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
        tui->input_win = newwin(INPUT_WIN_HEIGHT, max_x, max_y - INPUT_WIN_HEIGHT, 0);
        
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
