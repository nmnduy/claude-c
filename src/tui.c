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
#include "logger.h"
#include "indicators.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>
#include <ncurses.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include "message_queue.h"

#define INITIAL_CONV_CAPACITY 1000
#define INPUT_BUFFER_SIZE 8192
#define INPUT_WIN_MIN_HEIGHT 3  // Min height for input window (1 line + 2 borders)
#define INPUT_WIN_MAX_HEIGHT 5  // Max height for input window (3 lines + 2 borders)
#define CONV_WIN_PADDING 1      // Lines of padding between conv window and input window
#define STATUS_WIN_HEIGHT 1     // Single-line status window
#define TUI_MAX_MESSAGES_PER_FRAME 10  // Max messages processed per frame

// Global flag to detect terminal resize
static volatile sig_atomic_t g_resize_flag = 0;

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

// Render the status window based on current state
static const spinner_variant_t* status_spinner_variant(void) {
    init_global_spinner_variant();
    if (GLOBAL_SPINNER_VARIANT.frames && GLOBAL_SPINNER_VARIANT.count > 0) {
        return &GLOBAL_SPINNER_VARIANT;
    }
    static const spinner_variant_t fallback_variant = { SPINNER_FRAMES, SPINNER_FRAME_COUNT };
    return &fallback_variant;
}

static uint64_t status_spinner_interval_ns(void) {
    return (uint64_t)SPINNER_DELAY_MS * 1000000ULL;
}

static void render_status_window(TUIState *tui) {
    if (!tui || !tui->status_win) {
        return;
    }

    int height, width;
    getmaxyx(tui->status_win, height, width);
    (void)height;

    werase(tui->status_win);

    if (tui->status_visible && tui->status_message && tui->status_message[0] != '\0') {
        if (has_colors()) {
            wattron(tui->status_win, COLOR_PAIR(NCURSES_PAIR_STATUS) | A_BOLD);
        } else {
            wattron(tui->status_win, A_BOLD);
        }

        int col = 0;
        if (tui->status_spinner_active) {
            const spinner_variant_t *variant = status_spinner_variant();
            int frame_count = variant->count;
            const char **frames = variant->frames;
            if (!frames || frame_count <= 0) {
                frames = SPINNER_FRAMES;
                frame_count = SPINNER_FRAME_COUNT;
            }
            const char *frame = frames[tui->status_spinner_frame % frame_count];
            if (width > 0) {
                mvwaddnstr(tui->status_win, 0, col, frame, width - col);
                col += 1;
            }
            if (col < width) {
                mvwaddch(tui->status_win, 0, col, ' ');
                col += 1;
            }
        }

        if (col < width) {
            mvwaddnstr(tui->status_win, 0, col, tui->status_message, width - col);
        }

        if (has_colors()) {
            wattroff(tui->status_win, COLOR_PAIR(NCURSES_PAIR_STATUS) | A_BOLD);
        } else {
            wattroff(tui->status_win, A_BOLD);
        }
    }

    wrefresh(tui->status_win);
}

static uint64_t monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int status_message_wants_spinner(const char *message) {
    if (!message) {
        return 0;
    }
    if (strstr(message, "...")) {
        return 1;
    }
    if (strstr(message, "\xE2\x80\xA6")) { // Unicode ellipsis
        return 1;
    }
    return 0;
}

static void status_spinner_start(TUIState *tui) {
    if (!tui) {
        return;
    }
    if (!tui->status_spinner_active) {
        tui->status_spinner_frame = 0;
    }
    tui->status_spinner_active = 1;
    tui->status_spinner_last_update_ns = monotonic_time_ns();
}

static void status_spinner_stop(TUIState *tui) {
    if (!tui) {
        return;
    }
    tui->status_spinner_active = 0;
    tui->status_spinner_frame = 0;
    tui->status_spinner_last_update_ns = 0;
}

static void status_spinner_tick(TUIState *tui) {
    if (!tui || !tui->status_spinner_active || !tui->status_visible) {
        return;
    }
    if (tui->status_height <= 0 || !tui->status_win) {
        return;
    }

    uint64_t now = monotonic_time_ns();
    if (tui->status_spinner_last_update_ns == 0) {
        tui->status_spinner_last_update_ns = now;
        return;
    }

    uint64_t delta = now - tui->status_spinner_last_update_ns;
    uint64_t interval_ns = status_spinner_interval_ns();
    if (delta < interval_ns) {
        return;
    }

    uint64_t steps = interval_ns ? delta / interval_ns : 1;
    if (steps == 0) {
        steps = 1;
    }

    const spinner_variant_t *variant = status_spinner_variant();
    int frame_count = (variant->count > 0) ? variant->count : SPINNER_FRAME_COUNT;
    if (frame_count <= 0) {
        return;
    }
    tui->status_spinner_frame = (tui->status_spinner_frame + (int)steps) % frame_count;
    tui->status_spinner_last_update_ns = now;
    render_status_window(tui);
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

// Helper: Add a conversation entry to the TUI state
static int add_conversation_entry(TUIState *tui, const char *prefix, const char *text, TUIColorPair color_pair) {
    if (!tui) return -1;

    // Ensure capacity
    if (tui->entries_count >= tui->entries_capacity) {
        int new_capacity = tui->entries_capacity == 0 ? INITIAL_CONV_CAPACITY : tui->entries_capacity * 2;
        ConversationEntry *new_entries = realloc(tui->entries, (size_t)new_capacity * sizeof(ConversationEntry));
        if (!new_entries) {
            LOG_ERROR("[TUI] Failed to allocate memory for conversation entries");
            return -1;
        }
        tui->entries = new_entries;
        tui->entries_capacity = new_capacity;
    }

    // Allocate and copy strings
    ConversationEntry *entry = &tui->entries[tui->entries_count];
    entry->prefix = prefix ? strdup(prefix) : NULL;
    entry->text = text ? strdup(text) : NULL;
    entry->color_pair = color_pair;

    if ((prefix && !entry->prefix) || (text && !entry->text)) {
        free(entry->prefix);
        free(entry->text);
        LOG_ERROR("[TUI] Failed to allocate memory for conversation entry strings");
        return -1;
    }

    tui->entries_count++;
    return 0;
}

// Helper: Free all conversation entries
static void free_conversation_entries(TUIState *tui) {
    if (!tui || !tui->entries) return;

    for (int i = 0; i < tui->entries_count; i++) {
        free(tui->entries[i].prefix);
        free(tui->entries[i].text);
    }
    free(tui->entries);
    tui->entries = NULL;
    tui->entries_count = 0;
    tui->entries_capacity = 0;
}

// Helper: Render conversation window (simple version - one entry per line)
static void render_conversation_window(TUIState *tui) {
    if (!tui || !tui->conv_win) return;

    werase(tui->conv_win);

    int max_y, max_x;
    getmaxyx(tui->conv_win, max_y, max_x);

    // Simple rendering: display entries from scroll offset
    int start_entry = tui->conv_scroll_offset;
    if (start_entry < 0) start_entry = 0;
    if (start_entry >= tui->entries_count) start_entry = tui->entries_count - max_y;
    if (start_entry < 0) start_entry = 0;

    int y = 0;
    for (int i = start_entry; i < tui->entries_count && y < max_y; i++) {
        ConversationEntry *entry = &tui->entries[i];
        
        // Map TUIColorPair to ncurses color pair
        int mapped_pair = NCURSES_PAIR_FOREGROUND;
        switch (entry->color_pair) {
            case COLOR_PAIR_DEFAULT:
            case COLOR_PAIR_FOREGROUND:
                mapped_pair = NCURSES_PAIR_FOREGROUND;
                break;
            case COLOR_PAIR_USER:
                mapped_pair = NCURSES_PAIR_USER;
                break;
            case COLOR_PAIR_ASSISTANT:
                mapped_pair = NCURSES_PAIR_ASSISTANT;
                break;
            case COLOR_PAIR_TOOL:
            case COLOR_PAIR_STATUS:
                mapped_pair = NCURSES_PAIR_STATUS;
                break;
            case COLOR_PAIR_ERROR:
                mapped_pair = NCURSES_PAIR_ERROR;
                break;
            case COLOR_PAIR_PROMPT:
                mapped_pair = NCURSES_PAIR_PROMPT;
                break;
        }

        const int prefix_pair = mapped_pair;
        int text_pair = NCURSES_PAIR_FOREGROUND;
        int prefix_has_text = entry->prefix && entry->prefix[0] != '\0';

        if (!prefix_has_text &&
            entry->color_pair != COLOR_PAIR_DEFAULT &&
            entry->color_pair != COLOR_PAIR_FOREGROUND) {
            text_pair = mapped_pair;
        }

        // Move to start of line
        wmove(tui->conv_win, y, 0);
        
        // Print prefix with color
        if (prefix_has_text) {
            if (has_colors()) {
                wattron(tui->conv_win, COLOR_PAIR(prefix_pair) | A_BOLD);
            }
            wprintw(tui->conv_win, "%s", entry->prefix);
            if (has_colors()) {
                wattroff(tui->conv_win, COLOR_PAIR(prefix_pair) | A_BOLD);
            }
        }

        // Print text with foreground color
        if (entry->text) {
            if (has_colors()) {
                wattron(tui->conv_win, COLOR_PAIR(text_pair));
            }
            
            // Add space between prefix and text if both exist
            if (prefix_has_text) {
                wprintw(tui->conv_win, " ");
            }
            
            // Print text, truncating if too long
            int remaining = max_x - (prefix_has_text ? (int)strlen(entry->prefix) + 1 : 0);
            if (remaining > 0) {
                // Simple truncation - print up to remaining chars
                int text_len = (int)strlen(entry->text);
                if (text_len <= remaining) {
                    wprintw(tui->conv_win, "%s", entry->text);
                } else {
                    // Print truncated with ellipsis
                    char truncated[4096];
                    int copy_len = remaining - 3;  // Leave room for "..."
                    if (copy_len < 0) copy_len = 0;
                    if (copy_len > (int)sizeof(truncated) - 4) copy_len = (int)sizeof(truncated) - 4;
                    
                    strncpy(truncated, entry->text, (size_t)copy_len);
                    truncated[copy_len] = '\0';
                    strcat(truncated, "...");
                    wprintw(tui->conv_win, "%s", truncated);
                }
            }
            
            if (has_colors()) {
                wattroff(tui->conv_win, COLOR_PAIR(text_pair));
            }
        }

        y++;
    }

    wrefresh(tui->conv_win);
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
struct TUIInputBuffer {
    char *buffer;
    size_t capacity;
    int length;
    int cursor;
    WINDOW *win;
    int win_width;
    int win_height;
    // Display state
    int view_offset;         // Horizontal scroll offset for long lines
    int line_scroll_offset;  // Vertical scroll offset (which line to show at top)
};

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

    // Get screen dimensions
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    
    // Update input height
    tui->input_height = new_window_height;
    
    // Recalculate conversation window height
    int new_conv_height = max_y - new_window_height - tui->status_height - CONV_WIN_PADDING;
    if (new_conv_height < 5) new_conv_height = 5;

    // Resize conversation window if height changed
    if (new_conv_height != tui->conv_height) {
        tui->conv_height = new_conv_height;
        if (tui->conv_win) {
            delwin(tui->conv_win);
            tui->conv_win = newwin(new_conv_height, max_x, 0, 0);
            if (tui->conv_win) {
                scrollok(tui->conv_win, TRUE);
                render_conversation_window(tui);
            }
        }
    }

    // Recreate status window to account for new offsets
    if (tui->status_win) {
        delwin(tui->status_win);
        tui->status_win = NULL;
    }
    if (tui->status_height > 0) {
        tui->status_win = newwin(tui->status_height, max_x, tui->conv_height, 0);
        if (tui->status_win) {
            render_status_window(tui);
        }
    }

    // Delete old input window
    if (tui->input_win) {
        delwin(tui->input_win);
    }

    // Create new input window with adjusted height
    tui->input_win = newwin(new_window_height, max_x, max_y - new_window_height, 0);

    if (!tui->input_win) {
        LOG_ERROR("Failed to resize input window");
        return -1;
    }

    // Update input state
    if (tui->input_buffer) {
        tui->input_buffer->win = tui->input_win;
        int h, w;
        getmaxyx(tui->input_win, h, w);
        tui->input_buffer->win_width = w - 2;
        tui->input_buffer->win_height = h - 2;
    }

    // Enable keypad for new window
    keypad(tui->input_win, TRUE);

    // Refresh stdscr to clear old area
    touchwin(stdscr);
    if (tui->status_height > 0) {
        render_status_window(tui);
    }
    refresh();

    LOG_DEBUG("[TUI] Input window resized (new_input_h=%d, conv_h=%d, status_h=%d)",
              tui->input_height, tui->conv_height, tui->status_height);

    return 0;
}

// Initialize input buffer
static int input_init(TUIState *tui) {
    if (!tui || !tui->input_win) {
        return -1;
    }

    TUIInputBuffer *input = calloc(1, sizeof(TUIInputBuffer));
    if (!input) {
        return -1;
    }

    input->buffer = malloc(INPUT_BUFFER_SIZE);
    if (!input->buffer) {
        free(input);
        return -1;
    }

    input->capacity = INPUT_BUFFER_SIZE;
    input->buffer[0] = '\0';
    input->length = 0;
    input->cursor = 0;
    input->win = tui->input_win;
    input->view_offset = 0;
    input->line_scroll_offset = 0;

    // Get window dimensions
    int h, w;
    getmaxyx(tui->input_win, h, w);
    input->win_width = w - 2;  // Account for borders
    input->win_height = h - 2;

    tui->input_buffer = input;
    return 0;
}

// Free input buffer
static void input_free(TUIState *tui) {
    if (!tui || !tui->input_buffer) {
        return;
    }

    free(tui->input_buffer->buffer);
    tui->input_buffer->buffer = NULL;
    tui->input_buffer->capacity = 0;
    tui->input_buffer->length = 0;
    tui->input_buffer->cursor = 0;

    free(tui->input_buffer);
    tui->input_buffer = NULL;
}

// Insert character(s) at cursor position
static int input_insert_char(TUIInputBuffer *input, const unsigned char *utf8_char, int char_bytes) {
    if (!input) {
        return -1;
    }

    if (input->length + char_bytes >= (int)input->capacity - 1) {
        return -1;  // Buffer full
    }

    // Make space for the new character(s)
    memmove(&input->buffer[input->cursor + char_bytes],
            &input->buffer[input->cursor],
            (size_t)(input->length - input->cursor + 1));

    // Copy the character bytes
    for (int i = 0; i < char_bytes; i++) {
        input->buffer[input->cursor + i] = (char)utf8_char[i];
    }

    input->length += char_bytes;
    input->cursor += char_bytes;
    return 0;
}

// Delete character at cursor position (forward delete)
static int input_delete_char(TUIInputBuffer *input) {
    if (!input || input->cursor >= input->length) {
        return 0;  // Nothing to delete
    }

    // Find the length of the UTF-8 character at cursor
    int char_len = utf8_char_length((unsigned char)input->buffer[input->cursor]);

    // Delete the character by moving subsequent text left
    memmove(&input->buffer[input->cursor],
            &input->buffer[input->cursor + char_len],
            (size_t)(input->length - input->cursor - char_len + 1));

    input->length -= char_len;
    return char_len;
}

// Delete character before cursor (backspace)
static int input_backspace(TUIInputBuffer *input) {
    if (!input || input->cursor <= 0) {
        return 0;  // Nothing to delete
    }

    memmove(&input->buffer[input->cursor - 1],
            &input->buffer[input->cursor],
            (size_t)(input->length - input->cursor + 1));
    input->length--;
    input->cursor--;
    return 1;
}

// Delete word before cursor (Alt+Backspace)
static int input_delete_word_backward(TUIInputBuffer *input) {
    if (!input || input->cursor <= 0) {
        return 0;
    }

    int word_start = input->cursor - 1;
    while (word_start > 0 && is_word_boundary(input->buffer[word_start])) {
        word_start--;
    }
    while (word_start > 0 && !is_word_boundary(input->buffer[word_start])) {
        word_start--;
    }
    if (word_start > 0 && is_word_boundary(input->buffer[word_start])) {
        word_start++;
    }

    int delete_count = input->cursor - word_start;
    if (delete_count > 0) {
        memmove(&input->buffer[word_start],
                &input->buffer[input->cursor],
                (size_t)(input->length - input->cursor + 1));
        input->length -= delete_count;
        input->cursor = word_start;
    }

    return delete_count;
}

// Delete word forward (Alt+d)
static int input_delete_word_forward(TUIInputBuffer *input) {
    if (!input || input->cursor >= input->length) {
        return 0;
    }

    int word_end = move_forward_word(input->buffer, input->cursor, input->length);
    int delete_count = word_end - input->cursor;

    if (delete_count > 0) {
        memmove(&input->buffer[input->cursor],
                &input->buffer[word_end],
                (size_t)(input->length - word_end + 1));
        input->length -= delete_count;
    }

    return delete_count;
}

// Redraw the input window
static void input_redraw(TUIState *tui, const char *prompt) {
    if (!tui || !tui->input_buffer) {
        return;
    }

    TUIInputBuffer *input = tui->input_buffer;
    WINDOW *win = input->win;
    if (!win) {
        return;
    }

    int prompt_len = (int)strlen(prompt) + 1;  // +1 for space after prompt

    // Calculate available width for text (window width - borders - prompt)
    int available_width = input->win_width - prompt_len;
    if (available_width < 10) available_width = 10;

    // Calculate how many lines we need to display all content
    int needed_lines = calculate_needed_lines(input->buffer, input->length, available_width, prompt_len);

    // Request window resize (this will be a no-op if size hasn't changed)
    resize_input_window(tui, needed_lines);
    input = tui->input_buffer;
    win = input->win;
    if (!win) {
        return;
    }

    // Recalculate available width in case window resized
    available_width = input->win_width - prompt_len;
    if (available_width < 10) available_width = 10;

    // Calculate cursor line position
    int cursor_line = 0;
    int cursor_col = prompt_len;
    for (int i = 0; i < input->cursor; i++) {
        if (input->buffer[i] == '\n') {
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
    int max_visible_lines = input->win_height;
    if (cursor_line < input->line_scroll_offset) {
        input->line_scroll_offset = cursor_line;
    } else if (cursor_line >= input->line_scroll_offset + max_visible_lines) {
        input->line_scroll_offset = cursor_line - max_visible_lines + 1;
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
    if (input->line_scroll_offset == 0) {
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

    for (int i = 0; i < input->length && screen_y <= input->win_height; i++) {
        // Skip lines before scroll offset
        if (current_line < input->line_scroll_offset) {
            if (input->buffer[i] == '\n') {
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
        char c = input->buffer[i];
        if (c == '\n') {
            mvwaddch(win, screen_y, screen_x, (unsigned char)'+' | A_DIM);
            screen_y++;
            current_line++;
            screen_x = 1;  // Reset to left edge (after border)
        } else {
            mvwaddch(win, screen_y, screen_x, c);
            screen_x++;

            // Check if we need to wrap
            int line_width = available_width;
            if (current_line == input->line_scroll_offset && input->line_scroll_offset == 0) {
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
    int cursor_screen_y = cursor_line - input->line_scroll_offset + 1;
    int cursor_screen_x = cursor_col + 1;  // +1 for border

    // Recalculate cursor_col relative to its line
    int temp_line = 0;
    int temp_col = (temp_line == 0) ? prompt_len : 0;
    for (int i = 0; i < input->cursor; i++) {
        if (input->buffer[i] == '\n') {
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
    if (cursor_screen_y >= 1 && cursor_screen_y <= input->win_height &&
        cursor_screen_x >= 1 && cursor_screen_x <= input->win_width) {
        wmove(win, cursor_screen_y, cursor_screen_x);
    }

    wrefresh(win);
}

int tui_init(TUIState *tui) {
    if (!tui) return -1;

    // Store global pointer for input resize callback
    // Set locale for UTF-8 support
    setlocale(LC_ALL, "");

    // Initialize ncurses
    initscr();
    cbreak();  // Disable line buffering
    noecho();  // Don't echo input
    nonl();    // Don't translate Enter to newline (allows distinguishing Enter from Ctrl+J)
    keypad(stdscr, TRUE);  // Enable function keys
    nodelay(stdscr, FALSE);  // Blocking input
    curs_set(2);  // Make cursor very visible (block cursor)

    // Initialize colors from colorscheme
    init_ncurses_colors();

    // Get screen dimensions
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    tui->screen_height = max_y;
    tui->screen_width = max_x;
    tui->input_height = INPUT_WIN_MIN_HEIGHT;  // Start with minimum height
    tui->status_height = STATUS_WIN_HEIGHT;

    // Ensure we have enough space for status window; disable if terminal too small
    if (max_y - tui->input_height - CONV_WIN_PADDING - tui->status_height < 5) {
        LOG_WARN("[TUI] Terminal height too small for status window, disabling status line");
        tui->status_height = 0;
    }

    // Calculate conversation window height (screen - input - status - padding)
    tui->conv_height = max_y - tui->input_height - tui->status_height - CONV_WIN_PADDING;
    if (tui->conv_height < 5) tui->conv_height = 5;  // Minimum height

    // Create conversation window (top of screen)
    tui->conv_win = newwin(tui->conv_height, max_x, 0, 0);
    if (!tui->conv_win) {
        endwin();
        return -1;
    }
    scrollok(tui->conv_win, TRUE);

    // Create status window if enabled
    if (tui->status_height > 0) {
        tui->status_win = newwin(tui->status_height, max_x, tui->conv_height, 0);
        if (!tui->status_win) {
            delwin(tui->conv_win);
            endwin();
            return -1;
        }
    } else {
        tui->status_win = NULL;
    }

    // Create input window (bottom of screen)
    tui->input_win = newwin(INPUT_WIN_MIN_HEIGHT, max_x, max_y - INPUT_WIN_MIN_HEIGHT, 0);
    if (!tui->input_win) {
        delwin(tui->conv_win);
        if (tui->status_win) {
            delwin(tui->status_win);
        }
        endwin();
        return -1;
    }

    // Initialize conversation entries
    tui->entries = NULL;
    tui->entries_count = 0;
    tui->entries_capacity = 0;
    tui->conv_scroll_offset = 0;
    tui->status_message = NULL;
    tui->status_visible = 0;
    tui->status_spinner_active = 0;
    tui->status_spinner_frame = 0;
    tui->status_spinner_last_update_ns = 0;

    // Initialize input buffer
    if (input_init(tui) != 0) {
        delwin(tui->conv_win);
        if (tui->status_win) {
            delwin(tui->status_win);
        }
        delwin(tui->input_win);
        endwin();
        return -1;
    }

    // Register resize handler (if available)
#ifdef SIGWINCH
    signal(SIGWINCH, handle_resize);
#endif

    tui->is_initialized = 1;

    LOG_DEBUG("[TUI] Initialized (screen=%dx%d, conv_h=%d, status_h=%d, input_h=%d)",
              tui->screen_width, tui->screen_height, tui->conv_height,
              tui->status_height, tui->input_height);

    if (tui->status_height > 0) {
        render_status_window(tui);
    }

    refresh();

    return 0;
}

void tui_cleanup(TUIState *tui) {
    if (!tui || !tui->is_initialized) return;

    // Free conversation entries
    free_conversation_entries(tui);

    // Free input state
    input_free(tui);

    // Free status message
    free(tui->status_message);
    tui->status_message = NULL;

    // Destroy status window
    if (tui->status_win) {
        delwin(tui->status_win);
        tui->status_win = NULL;
    }

    // Destroy windows
    if (tui->conv_win) {
        delwin(tui->conv_win);
        tui->conv_win = NULL;
    }
    if (tui->input_win) {
        delwin(tui->input_win);
        tui->input_win = NULL;
    }

    // End ncurses
    endwin();

    tui->is_initialized = 0;

    // Print a newline to ensure clean exit
    printf("\n");
    LOG_DEBUG("[TUI] Cleaned up ncurses resources");
    fflush(stdout);
}

void tui_add_conversation_line(TUIState *tui, const char *prefix, const char *text, TUIColorPair color_pair) {
    if (!tui || !tui->is_initialized) return;

    // Add entry to conversation history
    if (add_conversation_entry(tui, prefix, text, color_pair) != 0) {
        LOG_ERROR("[TUI] Failed to add conversation entry");
        return;
    }

    // Auto-scroll to bottom (show latest messages)
    tui->conv_scroll_offset = tui->entries_count - tui->conv_height;
    if (tui->conv_scroll_offset < 0) {
        tui->conv_scroll_offset = 0;
    }

    // Render the conversation window
    render_conversation_window(tui);

    if (tui->status_height > 0) {
        render_status_window(tui);
    }

    // Redraw input window to ensure it stays visible
    if (tui->input_win) {
        touchwin(tui->input_win);
        wrefresh(tui->input_win);
    }
}

void tui_update_status(TUIState *tui, const char *status_text) {
    if (!tui || !tui->is_initialized) return;

    const char *message = status_text ? status_text : "";
    LOG_DEBUG("[TUI] Status update requested: '%s'", message[0] ? message : "(clear)");

    if (message[0] == '\0') {
        status_spinner_stop(tui);
        tui->status_visible = 0;
        free(tui->status_message);
        tui->status_message = NULL;
        if (tui->status_height > 0) {
            render_status_window(tui);
        }
        return;
    }

    if (!tui->status_message || strcmp(tui->status_message, message) != 0) {
        char *copy = strdup(message);
        if (!copy) {
            LOG_ERROR("[TUI] Failed to allocate memory for status message");
            return;
        }
        free(tui->status_message);
        tui->status_message = copy;
    }

    if (status_message_wants_spinner(message)) {
        status_spinner_start(tui);
    } else {
        status_spinner_stop(tui);
    }

    tui->status_visible = 1;

    if (tui->status_height > 0) {
        render_status_window(tui);
    }
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

    // Free all conversation entries
    free_conversation_entries(tui);
    
    // Reset scroll offset
    tui->conv_scroll_offset = 0;

    // Clear and refresh the conversation window
    if (tui->conv_win) {
        werase(tui->conv_win);
        wrefresh(tui->conv_win);
    }

    // Add a system message indicating the clear
    tui_add_conversation_line(tui, "[System]", "Conversation history cleared", COLOR_PAIR_STATUS);
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

    int new_status_height = STATUS_WIN_HEIGHT;
    if (max_y - tui->input_height - CONV_WIN_PADDING - new_status_height < 5) {
        if (tui->status_height != 0) {
            LOG_WARN("[TUI] Resize reduced status window due to limited height");
        }
        new_status_height = 0;
    }
    tui->status_height = new_status_height;

    // Recalculate conversation window height
    tui->conv_height = max_y - tui->input_height - tui->status_height - CONV_WIN_PADDING;
    if (tui->conv_height < 5) tui->conv_height = 5;

    // Resize and reposition conversation window
    if (tui->conv_win) {
        delwin(tui->conv_win);
        tui->conv_win = newwin(tui->conv_height, max_x, 0, 0);
        if (tui->conv_win) {
            scrollok(tui->conv_win, TRUE);
            render_conversation_window(tui);
            wrefresh(tui->conv_win);  // Force refresh after render
        }
    }

    // Resize and reposition status window
    if (tui->status_win) {
        delwin(tui->status_win);
        tui->status_win = NULL;
    }
    if (tui->status_height > 0) {
        tui->status_win = newwin(tui->status_height, max_x, tui->conv_height, 0);
        if (tui->status_win) {
            render_status_window(tui);
        }
    }

    // Resize and reposition input window
    if (tui->input_win) {
        // Delete and recreate window to avoid ncurses resize issues
        delwin(tui->input_win);
        tui->input_win = newwin(tui->input_height, max_x, max_y - tui->input_height, 0);

        if (tui->input_win) {
            // Re-enable keypad for the new window
            keypad(tui->input_win, TRUE);

            // Update input state dimensions
            if (tui->input_buffer) {
                int h, w;
                getmaxyx(tui->input_win, h, w);
                tui->input_buffer->win_width = w - 2;
                tui->input_buffer->win_height = h - 2;
                tui->input_buffer->win = tui->input_win;
            }
            
            // Force redraw of input window
            touchwin(tui->input_win);
            wrefresh(tui->input_win);
        }
    }

    LOG_DEBUG("[TUI] Resize handled (screen=%dx%d, conv_h=%d, status_h=%d, input_h=%d)",
              tui->screen_width, tui->screen_height, tui->conv_height,
              tui->status_height, tui->input_height);

    // Final refresh to ensure everything is visible
    refresh();
    doupdate();  // Update physical screen
}

void tui_show_startup_banner(TUIState *tui, const char *version, const char *model, const char *working_dir) {
    if (!tui || !tui->is_initialized) return;

    // Format banner lines
    char line1[256];
    char line2[256];
    char line3[256];
    
    snprintf(line1, sizeof(line1), " ▐▛███▜▌   claude-c v%s", version);
    snprintf(line2, sizeof(line2), "▝▜█████▛▘  %s", model);
    snprintf(line3, sizeof(line3), "  ▘▘ ▝▝    %s", working_dir);

    // Add padding before mascot
    tui_add_conversation_line(tui, NULL, "", COLOR_PAIR_FOREGROUND);
    
    // Add banner lines to conversation window
    tui_add_conversation_line(tui, NULL, line1, COLOR_PAIR_ASSISTANT);
    tui_add_conversation_line(tui, NULL, line2, COLOR_PAIR_ASSISTANT);
    tui_add_conversation_line(tui, NULL, line3, COLOR_PAIR_ASSISTANT);
    tui_add_conversation_line(tui, NULL, "", COLOR_PAIR_FOREGROUND);  // Blank line
}


void tui_scroll_conversation(TUIState *tui, int direction) {
    if (!tui || !tui->is_initialized || !tui->conv_win) return;

    // Calculate max scroll offset
    int max_offset = tui->entries_count - tui->conv_height;
    if (max_offset < 0) max_offset = 0;

    // Update scroll offset
    tui->conv_scroll_offset += direction;

    // Clamp to valid range
    if (tui->conv_scroll_offset < 0) {
        tui->conv_scroll_offset = 0;
    } else if (tui->conv_scroll_offset > max_offset) {
        tui->conv_scroll_offset = max_offset;
    }

    // Re-render conversation window
    render_conversation_window(tui);

    // Refresh input window to keep cursor visible
    if (tui->input_win) {
        touchwin(tui->input_win);
        wrefresh(tui->input_win);
    }
}

// ============================================================================
// Phase 2: Non-blocking Input and Event Loop Implementation
// ============================================================================

int tui_poll_input(TUIState *tui) {
    if (!tui || !tui->is_initialized || !tui->input_win) {
        return ERR;
    }
    
    // Make wgetch() non-blocking temporarily
    nodelay(tui->input_win, TRUE);
    int ch = wgetch(tui->input_win);
    nodelay(tui->input_win, FALSE);
    
    return ch;
}

int tui_process_input_char(TUIState *tui, int ch, const char *prompt) {
    if (!tui || !tui->is_initialized || !tui->input_win) {
        return -1;
    }

    TUIInputBuffer *input = tui->input_buffer;
    if (!input) {
        return -1;
    }

    // Handle special keys
    if (ch == KEY_RESIZE) {
        tui_handle_resize(tui);
        render_conversation_window(tui);
        render_status_window(tui);
        input_redraw(tui, prompt);
        return 0;
    } else if (ch == 1) {  // Ctrl+A: beginning of line
        input->cursor = 0;
        input_redraw(tui, prompt);
    } else if (ch == 5) {  // Ctrl+E: end of line
        input->cursor = input->length;
        input_redraw(tui, prompt);
    } else if (ch == 4) {  // Ctrl+D: EOF
        return -1;
    } else if (ch == 11) {  // Ctrl+K: kill to end of line
        input->buffer[input->cursor] = '\0';
        input->length = input->cursor;
        input_redraw(tui, prompt);
    } else if (ch == 21) {  // Ctrl+U: kill to beginning of line
        if (input->cursor > 0) {
            memmove(input->buffer,
                    &input->buffer[input->cursor],
                    (size_t)(input->length - input->cursor + 1));
            input->length -= input->cursor;
            input->cursor = 0;
            input_redraw(tui, prompt);
        }
    } else if (ch == 12) {  // Ctrl+L: clear input
        input->buffer[0] = '\0';
        input->length = 0;
        input->cursor = 0;
        input_redraw(tui, prompt);
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {  // Backspace
        if (input_backspace(input) > 0) {
            input_redraw(tui, prompt);
        }
    } else if (ch == KEY_DC) {  // Delete key
        if (input_delete_char(input) > 0) {
            input_redraw(tui, prompt);
        }
    } else if (ch == KEY_LEFT) {  // Left arrow
        if (input->cursor > 0) {
            input->cursor--;
            input_redraw(tui, prompt);
        }
    } else if (ch == KEY_RIGHT) {  // Right arrow
        if (input->cursor < input->length) {
            input->cursor++;
            input_redraw(tui, prompt);
        }
    } else if (ch == KEY_HOME) {  // Home
        input->cursor = 0;
        input_redraw(tui, prompt);
    } else if (ch == KEY_END) {  // End
        input->cursor = input->length;
        input_redraw(tui, prompt);
    } else if (ch == KEY_PPAGE) {  // Page Up: scroll conversation up
        tui_scroll_conversation(tui, -10);
        input_redraw(tui, prompt);
    } else if (ch == KEY_NPAGE) {  // Page Down: scroll conversation down
        tui_scroll_conversation(tui, 10);
        input_redraw(tui, prompt);
    } else if (ch == KEY_UP) {  // Up arrow: scroll conversation up (1 line)
        tui_scroll_conversation(tui, -1);
        input_redraw(tui, prompt);
    } else if (ch == KEY_DOWN) {  // Down arrow: scroll conversation down (1 line)
        tui_scroll_conversation(tui, 1);
        input_redraw(tui, prompt);
    } else if (ch == 10) {  // Ctrl+J: insert newline
        unsigned char newline = '\n';
        if (input_insert_char(input, &newline, 1) == 0) {
            input_redraw(tui, prompt);
        }
    } else if (ch == 13) {  // Enter: submit
        return 1;  // Signal submission
    } else if (ch == 27) {  // ESC sequence (Alt key combinations)
        // Set nodelay to check for following character
        nodelay(tui->input_win, TRUE);
        int next_ch = wgetch(tui->input_win);
        nodelay(tui->input_win, FALSE);

        if (next_ch == ERR) {
            // Standalone ESC, ignore
            return 0;
        } else if (next_ch == 'b' || next_ch == 'B') {  // Alt+b: backward word
            input->cursor = move_backward_word(input->buffer, input->cursor);
            input_redraw(tui, prompt);
        } else if (next_ch == 'f' || next_ch == 'F') {  // Alt+f: forward word
            input->cursor = move_forward_word(input->buffer, input->cursor, input->length);
            input_redraw(tui, prompt);
        } else if (next_ch == 'd' || next_ch == 'D') {  // Alt+d: delete next word
            if (input_delete_word_forward(input) > 0) {
                input_redraw(tui, prompt);
            }
        } else if (next_ch == KEY_BACKSPACE || next_ch == 127 || next_ch == 8) {  // Alt+Backspace
            if (input_delete_word_backward(input) > 0) {
                input_redraw(tui, prompt);
            }
        }
    } else if (ch >= 32 && ch < 127) {  // Printable ASCII
        unsigned char c = (unsigned char)ch;
        if (input_insert_char(input, &c, 1) == 0) {
            input_redraw(tui, prompt);
        }
    } else if (ch >= 128) {  // UTF-8 multibyte character (basic support)
        unsigned char c = (unsigned char)ch;
        if (input_insert_char(input, &c, 1) == 0) {
            input_redraw(tui, prompt);
        }
    }

    return 0;  // Continue processing
}

const char* tui_get_input_buffer(TUIState *tui) {
    if (!tui || !tui->input_buffer || tui->input_buffer->length == 0) {
        return NULL;
    }
    return tui->input_buffer->buffer;
}

void tui_clear_input_buffer(TUIState *tui) {
    if (!tui || !tui->input_buffer) {
        return;
    }

    tui->input_buffer->buffer[0] = '\0';
    tui->input_buffer->length = 0;
    tui->input_buffer->cursor = 0;
    tui->input_buffer->view_offset = 0;
    tui->input_buffer->line_scroll_offset = 0;
}

void tui_redraw_input(TUIState *tui, const char *prompt) {
    input_redraw(tui, prompt);
}

static TUIColorPair infer_color_from_prefix(const char *prefix) {
    if (!prefix) {
        return COLOR_PAIR_DEFAULT;
    }
    if (strstr(prefix, "User")) {
        return COLOR_PAIR_USER;
    }
    if (strstr(prefix, "Assistant")) {
        return COLOR_PAIR_ASSISTANT;
    }
    if (strstr(prefix, "Tool")) {
        return COLOR_PAIR_TOOL;
    }
    if (strstr(prefix, "Error")) {
        return COLOR_PAIR_ERROR;
    }
    if (strstr(prefix, "Diff +")) {
        return COLOR_PAIR_USER;
    }
    if (strstr(prefix, "Diff -")) {
        return COLOR_PAIR_ERROR;
    }
    if (strstr(prefix, "Diff @")) {
        return COLOR_PAIR_STATUS;
    }
    if (strstr(prefix, "Diff ~")) {
        return COLOR_PAIR_FOREGROUND;
    }
    if (strstr(prefix, "Diff")) {
        return COLOR_PAIR_STATUS;
    }
    if (strstr(prefix, "System")) {
        return COLOR_PAIR_STATUS;
    }
    if (strstr(prefix, "Prompt")) {
        return COLOR_PAIR_PROMPT;
    }
    return COLOR_PAIR_DEFAULT;
}

static void dispatch_tui_message(TUIState *tui, TUIMessage *msg) {
    if (!tui || !msg) {
        return;
    }

    switch (msg->type) {
        case TUI_MSG_ADD_LINE: {
            if (!msg->text) {
                tui_add_conversation_line(tui, "", "", COLOR_PAIR_DEFAULT);
                break;
            }

            char *mutable_text = msg->text;
            const char *content = mutable_text;

            if (mutable_text[0] == '[') {
                char *close = strchr(mutable_text, ']');
                if (close) {
                    size_t prefix_len = (size_t)(close - mutable_text + 1);
                    char *prefix = malloc(prefix_len + 1);
                    if (prefix) {
                        memcpy(prefix, mutable_text, prefix_len);
                        prefix[prefix_len] = '\0';
                    }

                    const char *content_start = close + 1;
                    while (*content_start == ' ') {
                        content_start++;
                    }

                    const char *color_source = prefix ? prefix : mutable_text;
                    tui_add_conversation_line(
                        tui,
                        prefix ? prefix : "",
                        content_start,
                        infer_color_from_prefix(color_source));

                    free(prefix);
                    break;
                }
            }

            tui_add_conversation_line(tui, "", content, COLOR_PAIR_DEFAULT);
            break;
        }

        case TUI_MSG_STATUS:
            tui_update_status(tui, msg->text ? msg->text : "");
            break;

        case TUI_MSG_CLEAR:
            tui_clear_conversation(tui);
            break;

        case TUI_MSG_ERROR:
            tui_add_conversation_line(
                tui,
                "[Error]",
                msg->text ? msg->text : "Unknown error",
                COLOR_PAIR_ERROR);
            break;

        case TUI_MSG_TODO_UPDATE:
            // Placeholder for future TODO list integration
            break;
    }
}

static int process_tui_messages(TUIState *tui,
                                TUIMessageQueue *msg_queue,
                                int max_messages) {
    if (!tui || !msg_queue || max_messages <= 0) {
        return 0;
    }

    int processed = 0;
    TUIMessage msg = {0};

    while (processed < max_messages) {
        int rc = poll_tui_message(msg_queue, &msg);
        if (rc <= 0) {
            if (rc < 0) {
                LOG_WARN("[TUI] Failed to poll message queue");
            }
            break;
        }

        dispatch_tui_message(tui, &msg);
        free(msg.text);
        msg.text = NULL;
        processed++;
    }

    return processed;
}

int tui_event_loop(TUIState *tui, const char *prompt, 
                   InputSubmitCallback callback, void *user_data,
                   void *msg_queue_ptr) {
    if (!tui || !tui->is_initialized || !callback) {
        return -1;
    }

    TUIMessageQueue *msg_queue = (TUIMessageQueue *)msg_queue_ptr;
    int running = 1;
    const long frame_time_us = 16667;  // ~60 FPS (1/60 second in microseconds)
    
    // Clear input buffer at start
    tui_clear_input_buffer(tui);
    
    // Initial draw (ensure all windows reflect current size)
    render_conversation_window(tui);
    render_status_window(tui);
    tui_redraw_input(tui, prompt);
    
    while (running) {
        struct timespec frame_start;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        
        // 1. Check for resize
        if (g_resize_flag) {
            g_resize_flag = 0;
            tui_handle_resize(tui);
            render_conversation_window(tui);
            render_status_window(tui);
            tui_redraw_input(tui, prompt);
        }
        
        // 2. Poll for input (non-blocking)
        int ch = tui_poll_input(tui);
        if (ch != ERR) {
            int result = tui_process_input_char(tui, ch, prompt);
            if (result == 1) {
                // Enter pressed - submit input
                const char *input = tui_get_input_buffer(tui);
                if (input && strlen(input) > 0) {
                    LOG_DEBUG("[TUI] Submitting input (%zu bytes)", strlen(input));
                    // Call the callback
                    int callback_result = callback(input, user_data);
                    
                    // Clear input buffer after submission
                    tui_clear_input_buffer(tui);
                    tui_redraw_input(tui, prompt);
                    
                    // Check if callback wants to exit
                    if (callback_result != 0) {
                        LOG_DEBUG("[TUI] Callback requested exit (code=%d)", callback_result);
                        running = 0;
                    }
                }
            } else if (result == -1) {
                // EOF/quit signal
                LOG_DEBUG("[TUI] Input processing returned EOF/quit");
                running = 0;
            }
        }
        
        // 3. Process TUI message queue (if provided)
        if (msg_queue) {
            int messages_processed = process_tui_messages(tui, msg_queue, TUI_MAX_MESSAGES_PER_FRAME);
            if (messages_processed > 0) {
                LOG_DEBUG("[TUI] Processed %d queued message(s)", messages_processed);
                tui_redraw_input(tui, prompt);
            }
        }
        
        // Update spinner animation if active
        status_spinner_tick(tui);

        // 4. Sleep to maintain frame rate
        struct timespec frame_end;
        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        
        long elapsed_ns = (frame_end.tv_sec - frame_start.tv_sec) * 1000000000L +
                         (frame_end.tv_nsec - frame_start.tv_nsec);
        long elapsed_us = elapsed_ns / 1000;
        
        if (elapsed_us < frame_time_us) {
            usleep((useconds_t)(frame_time_us - elapsed_us));
        }
    }
    
    return 0;
}

void tui_drain_message_queue(TUIState *tui, const char *prompt, void *msg_queue_ptr) {
    if (!tui || !msg_queue_ptr) {
        return;
    }

    TUIMessageQueue *msg_queue = (TUIMessageQueue *)msg_queue_ptr;
    int processed = 0;

    do {
        processed = process_tui_messages(tui, msg_queue, TUI_MAX_MESSAGES_PER_FRAME);
        if (processed > 0) {
            if (prompt) {
                tui_redraw_input(tui, prompt);
            } else {
                tui_refresh(tui);
            }
        }
    } while (processed > 0);
}
