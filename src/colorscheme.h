#ifndef COLORSCHEME_H
#define COLORSCHEME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <ncurses.h>
#include <math.h>

// TUI color pair assignments
#define PAIR_ASSISTANT  1  // Assistant text
#define PAIR_USER       2  // User text
#define PAIR_STATUS     3  // Status bar
#define PAIR_HEADER     4  // Headers/info
#define PAIR_ERROR      5  // Errors

// RGB color structure (0-255 values)
typedef struct {
    int r;
    int g;
    int b;
} RGB;

// Theme structure to hold parsed Kitty colors
typedef struct {
    int assistant_fg;    // Assistant text color
    int user_fg;         // User text color
    int status_bg;       // Status bar color
    int error_fg;        // Error message color
    int header_fg;       // Header color
    int background;      // Background color
} Theme;

// Parse hex color "#RRGGBB" to RGB struct
static RGB parse_hex_color(const char *hex) {
    RGB rgb = {0, 0, 0};

    // Skip leading '#' if present
    if (hex[0] == '#') {
        hex++;
    }

    // Parse hex string
    if (strlen(hex) == 6) {
        char r_str[3] = {hex[0], hex[1], '\0'};
        char g_str[3] = {hex[2], hex[3], '\0'};
        char b_str[3] = {hex[4], hex[5], '\0'};

        rgb.r = (int)strtol(r_str, NULL, 16);
        rgb.g = (int)strtol(g_str, NULL, 16);
        rgb.b = (int)strtol(b_str, NULL, 16);

        fprintf(stderr, "[THEME]     Parsed hex %s -> RGB(%d, %d, %d)\n",
                hex - 1, rgb.r, rgb.g, rgb.b);
    }

    return rgb;
}

// Convert RGB (0-255) to ncurses color number (0-255)
// Uses the standard 256-color palette
static int rgb_to_ncurses_color(RGB rgb) {
    // For the 8 basic colors, use COLOR_* constants
    if (rgb.r == 0 && rgb.g == 0 && rgb.b == 0) return COLOR_BLACK;
    if (rgb.r >= 200 && rgb.g < 50 && rgb.b < 50) return COLOR_RED;
    if (rgb.r < 50 && rgb.g >= 200 && rgb.b < 50) return COLOR_GREEN;
    if (rgb.r >= 200 && rgb.g >= 200 && rgb.b < 50) return COLOR_YELLOW;
    if (rgb.r < 50 && rgb.g < 50 && rgb.b >= 200) return COLOR_BLUE;
    if (rgb.r >= 200 && rgb.g < 50 && rgb.b >= 200) return COLOR_MAGENTA;
    if (rgb.r < 50 && rgb.g >= 200 && rgb.b >= 200) return COLOR_CYAN;
    if (rgb.r >= 200 && rgb.g >= 200 && rgb.b >= 200) return COLOR_WHITE;

    // For 256-color terminals, use the extended palette
    // Colors 16-231 are a 6x6x6 RGB cube
    // Colors 232-255 are grayscale

    if (COLORS >= 256) {
        // Check if it's a grayscale color
        int avg = (rgb.r + rgb.g + rgb.b) / 3;
        int r_diff = abs(rgb.r - avg);
        int g_diff = abs(rgb.g - avg);
        int b_diff = abs(rgb.b - avg);

        if (r_diff < 10 && g_diff < 10 && b_diff < 10) {
            // Grayscale ramp: 232-255 (24 shades)
            int gray_index = (avg * 23) / 255;
            int color = 232 + gray_index;
            fprintf(stderr, "[THEME]     RGB(%d,%d,%d) -> grayscale color %d\n",
                    rgb.r, rgb.g, rgb.b, color);
            return color;
        }

        // 6x6x6 RGB cube (216 colors): 16-231
        int r_idx = (rgb.r * 5) / 255;
        int g_idx = (rgb.g * 5) / 255;
        int b_idx = (rgb.b * 5) / 255;
        int color = 16 + (36 * r_idx) + (6 * g_idx) + b_idx;

        fprintf(stderr, "[THEME]     RGB(%d,%d,%d) -> 256-color %d\n",
                rgb.r, rgb.g, rgb.b, color);
        return color;
    }

    // Fallback for 8-color terminals
    int r = rgb.r >= 128 ? 1 : 0;
    int g = rgb.g >= 128 ? 1 : 0;
    int b = rgb.b >= 128 ? 1 : 0;
    int color = r * COLOR_RED + g * COLOR_GREEN + b * COLOR_BLUE;

    fprintf(stderr, "[THEME]     RGB(%d,%d,%d) -> basic color %d\n",
            rgb.r, rgb.g, rgb.b, color);
    return color;
}

// Load Kitty theme from file
static int load_kitty_theme(const char *filepath, Theme *theme) {
    fprintf(stderr, "[THEME] Loading Kitty theme from: %s\n", filepath);

    FILE *f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "[THEME] ERROR: Failed to open file: %s\n", filepath);
        return 0;
    }

    fprintf(stderr, "[THEME] File opened successfully\n");

    // Initialize with defaults (will be overridden by theme)
    theme->assistant_fg = COLOR_BLUE;
    theme->user_fg = COLOR_GREEN;
    theme->status_bg = COLOR_YELLOW;
    theme->error_fg = COLOR_RED;
    theme->header_fg = COLOR_CYAN;
    theme->background = COLOR_BLACK;

    char line[1024];
    int line_num = 0;
    int parsed_count = 0;

    while (fgets(line, sizeof(line), f)) {
        line_num++;

        // Remove trailing newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') {
            line[len-1] = '\0';
        }

        // Skip empty lines and comments
        char *p = line;
        while (*p && isspace(*p)) p++;
        if (*p == '\0' || *p == '#') {
            continue;
        }

        // Parse key-value pair
        char key[64], value[32];
        if (sscanf(p, "%63s %31s", key, value) == 2) {
            fprintf(stderr, "[THEME] Line %d: %s = %s\n", line_num, key, value);

            RGB rgb = parse_hex_color(value);
            int color = rgb_to_ncurses_color(rgb);

            // Map Kitty color keys to TUI elements
            if (strcmp(key, "foreground") == 0 || strcmp(key, "assistant_fg") == 0) {
                theme->assistant_fg = color;
                parsed_count++;
                fprintf(stderr, "[THEME]   -> Set assistant_fg = %d\n", color);
            }
            else if (strcmp(key, "color2") == 0 || strcmp(key, "user_fg") == 0) {
                theme->user_fg = color;
                parsed_count++;
                fprintf(stderr, "[THEME]   -> Set user_fg = %d\n", color);
            }
            else if (strcmp(key, "color3") == 0 || strcmp(key, "status_bg") == 0) {
                theme->status_bg = color;
                parsed_count++;
                fprintf(stderr, "[THEME]   -> Set status_bg = %d\n", color);
            }
            else if (strcmp(key, "color1") == 0 || strcmp(key, "error_fg") == 0) {
                theme->error_fg = color;
                parsed_count++;
                fprintf(stderr, "[THEME]   -> Set error_fg = %d\n", color);
            }
            else if (strcmp(key, "color4") == 0 || strcmp(key, "color6") == 0 ||
                     strcmp(key, "header_fg") == 0) {
                theme->header_fg = color;
                parsed_count++;
                fprintf(stderr, "[THEME]   -> Set header_fg = %d\n", color);
            }
            else if (strcmp(key, "background") == 0) {
                theme->background = color;
                parsed_count++;
                fprintf(stderr, "[THEME]   -> Set background = %d\n", color);
            }
        }
    }

    fclose(f);
    fprintf(stderr, "[THEME] Parsed %d color mappings\n", parsed_count);

    return parsed_count > 0;
}

// Apply theme to ncurses
static void apply_theme(Theme *theme) {
    fprintf(stderr, "[THEME] Applying theme to ncurses\n");
    fprintf(stderr, "[THEME]   PAIR_ASSISTANT: fg=%d bg=%d\n", theme->assistant_fg, theme->background);
    fprintf(stderr, "[THEME]   PAIR_USER: fg=%d bg=%d\n", theme->user_fg, theme->background);
    fprintf(stderr, "[THEME]   PAIR_STATUS: fg=%d bg=%d\n", COLOR_BLACK, theme->status_bg);
    fprintf(stderr, "[THEME]   PAIR_HEADER: fg=%d bg=%d\n", theme->header_fg, theme->background);
    fprintf(stderr, "[THEME]   PAIR_ERROR: fg=%d bg=%d\n", theme->error_fg, theme->background);

    init_pair(PAIR_ASSISTANT, theme->assistant_fg, theme->background);
    init_pair(PAIR_USER, theme->user_fg, theme->background);
    init_pair(PAIR_STATUS, COLOR_BLACK, theme->status_bg);
    init_pair(PAIR_HEADER, theme->header_fg, theme->background);
    init_pair(PAIR_ERROR, theme->error_fg, theme->background);
}

// Initialize colorscheme system with optional Kitty theme file
// If filepath is NULL or file doesn't exist, uses default colors
static void init_colorscheme(const char *filepath) {
    fprintf(stderr, "[THEME] Initializing colorscheme system\n");

    if (!has_colors()) {
        fprintf(stderr, "[THEME] Terminal does not support colors\n");
        return;
    }

    start_color();
    fprintf(stderr, "[THEME] Color support enabled (COLORS=%d, COLOR_PAIRS=%d)\n",
            COLORS, COLOR_PAIRS);

    // Try to load custom Kitty theme
    if (filepath) {
        fprintf(stderr, "[THEME] Custom theme path provided: %s\n", filepath);
        Theme theme;
        if (load_kitty_theme(filepath, &theme)) {
            fprintf(stderr, "[THEME] Successfully loaded custom theme\n");
            apply_theme(&theme);
            return;
        }
        fprintf(stderr, "[THEME] Failed to load custom theme, falling back to defaults\n");
    } else {
        fprintf(stderr, "[THEME] No custom theme path provided\n");
    }

    // Fall back to default colors
    fprintf(stderr, "[THEME] Using default colors\n");
    init_pair(PAIR_ASSISTANT, COLOR_BLUE, COLOR_BLACK);
    init_pair(PAIR_USER, COLOR_GREEN, COLOR_BLACK);
    init_pair(PAIR_STATUS, COLOR_BLACK, COLOR_YELLOW);
    init_pair(PAIR_HEADER, COLOR_CYAN, COLOR_BLACK);
    init_pair(PAIR_ERROR, COLOR_RED, COLOR_BLACK);
}

#endif // COLORSCHEME_H
