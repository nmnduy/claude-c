/*
 * Stub implementations for TODO test suite
 *
 * Provides minimal implementations of dependencies needed by todo.c
 */

#include <stdio.h>
#include <stdarg.h>
#include "../src/logger.h"

// Colorscheme stubs - provide minimal theme support
typedef struct {
    unsigned int foreground_rgb;
    unsigned int user_rgb;
    unsigned int assistant_rgb;
    unsigned int error_rgb;
    unsigned int status_rgb;
    unsigned int diff_add_rgb;
    unsigned int diff_remove_rgb;
    unsigned int diff_header_rgb;
    unsigned int diff_context_rgb;
} Theme;

Theme g_theme = {
    .foreground_rgb = 0xFFFFFF,
    .user_rgb = 0x00FF00,
    .assistant_rgb = 0x00FFFF,
    .error_rgb = 0xFF0000,
    .status_rgb = 0xFFFF00,
    .diff_add_rgb = 0x00FF00,
    .diff_remove_rgb = 0xFF0000,
    .diff_header_rgb = 0x00FFFF,
    .diff_context_rgb = 0xAAAAAA,
};

int g_theme_loaded = 1;

// Logger stub - matches signature from logger.h
void log_message(LogLevel level, const char *file, int line,
                const char *func, const char *fmt, ...) {
    (void)level;
    (void)file;
    (void)line;
    (void)func;
    (void)fmt;
    // Suppress log output in tests
}
