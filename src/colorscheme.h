#ifndef COLORSCHEME_H
#define COLORSCHEME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "logger.h"
#include "fallback_colors.h"

// Colorscheme element types (for ANSI escape code generation)
typedef enum {
    COLORSCHEME_USER,
    COLORSCHEME_ASSISTANT,
    COLORSCHEME_TOOL,
    COLORSCHEME_ERROR,
    COLORSCHEME_STATUS
} ColorschemeElement;

// RGB color structure (0-255 values)
typedef struct {
    int r;
    int g;
    int b;
} RGB;

// Theme structure to hold parsed Kitty colors
typedef struct {
    RGB assistant_rgb;   // RGB values for ANSI codes
    RGB user_rgb;
    RGB status_rgb;
    RGB error_rgb;
    RGB header_rgb;
} Theme;

// Global theme state (non-static so it's shared across compilation units)
// Only one file should include this header in implementation mode
#ifndef COLORSCHEME_EXTERN
Theme g_theme = {0};
int g_theme_loaded = 0;
#else
extern Theme g_theme;
extern int g_theme_loaded;
#endif

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

        LOG_DEBUG("[THEME]     Parsed hex %s -> RGB(%d, %d, %d)",
                hex - 1, rgb.r, rgb.g, rgb.b);
    }

    return rgb;
}

// Convert RGB to ANSI 256-color escape code
// Returns foreground color code like "\033[38;5;123m"
static void rgb_to_ansi_code(RGB rgb, char *buf, size_t bufsize) {
    // Convert RGB to nearest 256-color palette index
    // Check if it's grayscale
    int avg = (rgb.r + rgb.g + rgb.b) / 3;
    int r_diff = abs(rgb.r - avg);
    int g_diff = abs(rgb.g - avg);
    int b_diff = abs(rgb.b - avg);

    int color_idx;
    if (r_diff < 10 && g_diff < 10 && b_diff < 10) {
        // Grayscale: use colors 232-255 (24 shades)
        int gray_index = (avg * 23) / 255;
        color_idx = 232 + gray_index;
    } else {
        // RGB cube: 16-231
        int r_idx = (rgb.r * 5) / 255;
        int g_idx = (rgb.g * 5) / 255;
        int b_idx = (rgb.b * 5) / 255;
        color_idx = 16 + (36 * r_idx) + (6 * g_idx) + b_idx;
    }

    snprintf(buf, bufsize, "\033[38;5;%dm", color_idx);
}

// Get ANSI color code for a colorscheme element
// Returns 0 on success, -1 if no theme loaded
static int get_colorscheme_color(ColorschemeElement element, char *buf, size_t bufsize) {
    if (!g_theme_loaded) {
        return -1;  // No theme loaded
    }

    RGB rgb = {0, 0, 0};
    switch (element) {
        case COLORSCHEME_USER:
            rgb = g_theme.user_rgb;
            break;
        case COLORSCHEME_ASSISTANT:
            rgb = g_theme.assistant_rgb;
            break;
        case COLORSCHEME_TOOL:
            rgb = g_theme.status_rgb;  // Use status color for tools
            break;
        case COLORSCHEME_ERROR:
            rgb = g_theme.error_rgb;
            break;
        case COLORSCHEME_STATUS:
            rgb = g_theme.status_rgb;
            break;
        default:
            return -1;
    }

    rgb_to_ansi_code(rgb, buf, bufsize);
    return 0;
}

// Load Kitty theme from file
static int load_kitty_theme(const char *filepath, Theme *theme) {
    LOG_INFO("[THEME] Loading Kitty theme from: %s", filepath);

    FILE *f = fopen(filepath, "r");
    if (!f) {
        LOG_ERROR("[THEME] ERROR: Failed to open file: %s", filepath);
        return 0;
    }

    LOG_DEBUG("[THEME] File opened successfully");

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
            LOG_DEBUG("[THEME] Line %d: %s = %s", line_num, key, value);

            RGB rgb = parse_hex_color(value);

            // Map Kitty color keys to TUI elements
            if (strcmp(key, "foreground") == 0 || strcmp(key, "assistant_fg") == 0) {
                theme->assistant_rgb = rgb;
                parsed_count++;
                LOG_DEBUG("[THEME]   -> Set assistant_rgb = RGB(%d,%d,%d)", rgb.r, rgb.g, rgb.b);
            }
            else if (strcmp(key, "color2") == 0 || strcmp(key, "user_fg") == 0) {
                theme->user_rgb = rgb;
                parsed_count++;
                LOG_DEBUG("[THEME]   -> Set user_rgb = RGB(%d,%d,%d)", rgb.r, rgb.g, rgb.b);
            }
            else if (strcmp(key, "color3") == 0 || strcmp(key, "status_bg") == 0) {
                theme->status_rgb = rgb;
                parsed_count++;
                LOG_DEBUG("[THEME]   -> Set status_rgb = RGB(%d,%d,%d)", rgb.r, rgb.g, rgb.b);
            }
            else if (strcmp(key, "color1") == 0 || strcmp(key, "error_fg") == 0) {
                theme->error_rgb = rgb;
                parsed_count++;
                LOG_DEBUG("[THEME]   -> Set error_rgb = RGB(%d,%d,%d)", rgb.r, rgb.g, rgb.b);
            }
            else if (strcmp(key, "color4") == 0 || strcmp(key, "color6") == 0 ||
                     strcmp(key, "header_fg") == 0) {
                theme->header_rgb = rgb;
                parsed_count++;
                LOG_DEBUG("[THEME]   -> Set header_rgb = RGB(%d,%d,%d)", rgb.r, rgb.g, rgb.b);
            }
        }
    }

    fclose(f);
    LOG_DEBUG("[THEME] Parsed %d color mappings", parsed_count);

    return parsed_count > 0;
}

// Initialize colorscheme system with optional Kitty theme file
// If filepath is NULL or file doesn't exist, uses default colors
// Returns 0 on success, -1 on failure
// Note: This doesn't require ncurses - works with raw ANSI codes
static int init_colorscheme(const char *filepath) {
    LOG_DEBUG("[THEME] Initializing colorscheme system");

    // Try to load custom Kitty theme
    if (filepath) {
        LOG_DEBUG("[THEME] Custom theme path provided: %s", filepath);
        if (load_kitty_theme(filepath, &g_theme)) {
            LOG_DEBUG("[THEME] Successfully loaded custom theme");
            g_theme_loaded = 1;
            return 0;
        }
        LOG_WARN("[THEME] Failed to load custom theme, falling back to defaults");
    } else {
        LOG_DEBUG("[THEME] No custom theme path provided");
    }

    // Fall back to default colors
    LOG_DEBUG("[THEME] Using default colors");

    // Set default RGB values for ANSI codes
    g_theme.assistant_rgb = (RGB){100, 149, 237};   // Cornflower blue
    g_theme.user_rgb = (RGB){80, 250, 123};         // Bright green
    g_theme.status_rgb = (RGB){241, 250, 140};      // Yellow
    g_theme.error_rgb = (RGB){255, 85, 85};         // Red
    g_theme.header_rgb = (RGB){139, 233, 253};      // Cyan
    g_theme_loaded = 1;

    return 0;
}

#endif // COLORSCHEME_H
