/*
 * TUI (Terminal User Interface) - Simple terminal interface
 */

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
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define INITIAL_CONV_CAPACITY 1000
#define INPUT_BUFFER_SIZE 8192

// Global spinner for TUI status updates
static Spinner *g_tui_spinner = NULL;





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

    // Stop any running spinner
    if (g_tui_spinner) {
        spinner_stop(g_tui_spinner, NULL, 1);
        g_tui_spinner = NULL;
    }

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

    // Get color from colorscheme or fall back to centralized defaults
    const char *color_start = "";
    const char *color_end = ANSI_RESET;
    char color_code[32];

    // Try to get color from colorscheme first
    switch (color_pair) {
        case COLOR_PAIR_USER:
            if (get_colorscheme_color(COLORSCHEME_USER, color_code, sizeof(color_code)) == 0) {
                color_start = color_code;
            } else {
                color_start = ANSI_FALLBACK_USER;  // Green fallback from centralized system
            }
            break;
        case COLOR_PAIR_ASSISTANT:
            if (get_colorscheme_color(COLORSCHEME_ASSISTANT, color_code, sizeof(color_code)) == 0) {
                color_start = color_code;
            } else {
                color_start = ANSI_FALLBACK_ASSISTANT;  // Blue fallback from centralized system
            }
            break;
        case COLOR_PAIR_TOOL:
            if (get_colorscheme_color(COLORSCHEME_TOOL, color_code, sizeof(color_code)) == 0) {
                color_start = color_code;
            } else {
                color_start = ANSI_FALLBACK_TOOL;  // Yellow fallback from centralized system
            }
            break;
        case COLOR_PAIR_ERROR:
            if (get_colorscheme_color(COLORSCHEME_ERROR, color_code, sizeof(color_code)) == 0) {
                color_start = color_code;
            } else {
                color_start = ANSI_FALLBACK_ERROR;  // Red fallback from centralized system
            }
            break;
        case COLOR_PAIR_STATUS:
            if (get_colorscheme_color(COLORSCHEME_STATUS, color_code, sizeof(color_code)) == 0) {
                color_start = color_code;
            } else {
                color_start = ANSI_FALLBACK_STATUS;  // Cyan fallback from centralized system
            }
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

    // If status text is empty, stop any running spinner and clear the line
    if (!status_text || strlen(status_text) == 0) {
        if (g_tui_spinner) {
            spinner_stop(g_tui_spinner, NULL, 1);  // Clear without message
            g_tui_spinner = NULL;
        }
        return;
    }

    // If we have a running spinner, update its message
    if (g_tui_spinner) {
        spinner_update(g_tui_spinner, status_text);
    } else {
        // Start a new spinner with the status message
        g_tui_spinner = spinner_start(status_text, SPINNER_CYAN);
    }
}

char* tui_read_input(TUIState *tui, const char *prompt) {
    if (!tui || !tui->is_initialized) return NULL;

    // Stop any running spinner before showing input prompt
    if (g_tui_spinner) {
        spinner_stop(g_tui_spinner, NULL, 1);
        g_tui_spinner = NULL;
    }

    // Use lineedit for input handling - simple and works with terminal scrolling
    LineEditor editor;
    lineedit_init(&editor, NULL, NULL);  // No completion for now

    // Get prompt color from colorscheme or use centralized fallback green
    char color_code[32];
    const char *color_start;
    if (get_colorscheme_color(COLORSCHEME_USER, color_code, sizeof(color_code)) == 0) {
        color_start = color_code;
    } else {
        color_start = ANSI_FALLBACK_USER;  // Green fallback from centralized system
    }

    // Create colored prompt
    char colored_prompt[256];
    snprintf(colored_prompt, sizeof(colored_prompt), "%s%s%s ", color_start, prompt, ANSI_RESET);

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

    // Get status color from colorscheme or use centralized fallback cyan
    char color_code[32];
    const char *color_start;
    if (get_colorscheme_color(COLORSCHEME_STATUS, color_code, sizeof(color_code)) == 0) {
        color_start = color_code;
    } else {
        color_start = ANSI_FALLBACK_STATUS;  // Cyan fallback from centralized system
    }

    printf("%s[System] Conversation history cleared (kept in terminal scrollback)%s\n", color_start, ANSI_RESET);
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

    // Get assistant color from colorscheme (appropriate for the Claude mascot)
    char color_code[32];
    const char *color_start;
    if (get_colorscheme_color(COLORSCHEME_ASSISTANT, color_code, sizeof(color_code)) == 0) {
        color_start = color_code;
    } else {
        color_start = ANSI_FALLBACK_BOLD_CYAN;  // Bold cyan fallback from centralized system
    }

    // Print banner with colorscheme color
    printf("%s", color_start);  // Use colorscheme color
    printf(" ▐▛███▜▌   claude-c v%s\n", version);
    printf("▝▜█████▛▘  %s\n", model);
    printf("  ▘▘ ▝▝    %s\n", working_dir);
    printf(ANSI_RESET "\n");  // Reset color and add blank line
    fflush(stdout);
}

void tui_render_todo_list(TUIState *tui, const struct TodoList *todo_list) {
    if (!tui || !tui->is_initialized || !todo_list) return;

    // Stop any running spinner before rendering TODO list
    if (g_tui_spinner) {
        spinner_stop(g_tui_spinner, NULL, 1);
        g_tui_spinner = NULL;
    }

    // We need to include todo.h types, but to avoid circular dependencies,
    // we'll call the todo_render function directly which handles the formatting
    // For now, we'll just forward to it
    extern void todo_render(const struct TodoList *list);
    todo_render(todo_list);
}
