/*
 * indicators.h - Visual indicators for tool execution and API calls
 *
 * Provides animated spinners and status indicators for GPU-accelerated terminals
 * Supports Unicode characters and smooth ANSI animations
 */

#ifndef INDICATORS_H
#define INDICATORS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

// Spinner animation frames using Unicode characters
// These render beautifully in GPU-accelerated terminals like Kitty, Alacritty, WezTerm
static const char *SPINNER_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"  // Braille dots
};
static const int SPINNER_FRAME_COUNT = 10;
static const int SPINNER_DELAY_MS = 80;  // 80ms per frame = smooth animation

// Alternative spinners for variety
static const char *SPINNER_DOTS[] = {"⣾", "⣽", "⣻", "⢿", "⡿", "⣟", "⣯", "⣷"};
static const char *SPINNER_LINE[] = {"-", "\\", "|", "/"};
static const char *SPINNER_BOX[] = {"◰", "◳", "◲", "◱"};
static const char *SPINNER_CIRCLE[] = {"◜", "◠", "◝", "◞", "◡", "◟"};

// Color codes
#define SPINNER_CYAN "\033[36m"
#define SPINNER_YELLOW "\033[33m"
#define SPINNER_GREEN "\033[32m"
#define SPINNER_BLUE "\033[34m"
#define SPINNER_RESET "\033[0m"

typedef struct {
    pthread_t thread;
    int running;
    char *message;
    const char *color;
    pthread_mutex_t lock;
} Spinner;

static void *spinner_thread_func(void *arg) {
    Spinner *spinner = (Spinner *)arg;
    int frame = 0;

    // Hide cursor for smooth animation
    printf("\033[?25l");
    fflush(stdout);

    while (1) {
        pthread_mutex_lock(&spinner->lock);
        if (!spinner->running) {
            pthread_mutex_unlock(&spinner->lock);
            break;
        }

        // Clear line and redraw spinner with message
        printf("\r\033[K%s%s%s %s",
               spinner->color,
               SPINNER_FRAMES[frame],
               SPINNER_RESET,
               spinner->message);
        fflush(stdout);

        pthread_mutex_unlock(&spinner->lock);

        frame = (frame + 1) % SPINNER_FRAME_COUNT;
        usleep(SPINNER_DELAY_MS * 1000);
    }

    // Show cursor again
    printf("\033[?25h");
    fflush(stdout);

    return NULL;
}

// Start a spinner with a message
static Spinner* spinner_start(const char *message, const char *color) {
    Spinner *spinner = malloc(sizeof(Spinner));
    if (!spinner) return NULL;

    spinner->message = strdup(message);
    spinner->color = color ? color : SPINNER_CYAN;
    spinner->running = 1;
    pthread_mutex_init(&spinner->lock, NULL);

    pthread_create(&spinner->thread, NULL, spinner_thread_func, spinner);

    return spinner;
}

// Update spinner message
static void spinner_update(Spinner *spinner, const char *new_message) {
    if (!spinner) return;

    pthread_mutex_lock(&spinner->lock);
    free(spinner->message);
    spinner->message = strdup(new_message);
    pthread_mutex_unlock(&spinner->lock);
}

// Stop spinner and optionally show success/failure message
static void spinner_stop(Spinner *spinner, const char *final_message, int success) {
    if (!spinner) return;

    pthread_mutex_lock(&spinner->lock);
    spinner->running = 0;
    pthread_mutex_unlock(&spinner->lock);

    pthread_join(spinner->thread, NULL);

    // Clear line
    printf("\r\033[K");

    // Print final message if provided
    if (final_message) {
        if (success) {
            printf("%s✓%s %s\n", SPINNER_GREEN, SPINNER_RESET, final_message);
        } else {
            printf("%s✗%s %s\n", "\033[31m", SPINNER_RESET, final_message);
        }
    }

    fflush(stdout);

    pthread_mutex_destroy(&spinner->lock);
    free(spinner->message);
    free(spinner);
}

// Simple progress indicator for tool execution
static void tool_indicator_start(const char *tool_name) {
    printf("%s▸%s Running %s%s%s...\n",
           SPINNER_YELLOW,
           SPINNER_RESET,
           SPINNER_CYAN,
           tool_name,
           SPINNER_RESET);
    fflush(stdout);
}

static void tool_indicator_done(const char *tool_name, int success) {
    if (success) {
        printf("%s✓%s %s completed\n", SPINNER_GREEN, SPINNER_RESET, tool_name);
    } else {
        printf("%s✗%s %s failed\n", "\033[31m", SPINNER_RESET, tool_name);
    }
    fflush(stdout);
}

// Inline spinner for short operations (no separate thread)
static void inline_spinner_frame(int frame, const char *message, const char *color) {
    printf("\r\033[K%s%s%s %s",
           color ? color : SPINNER_CYAN,
           SPINNER_FRAMES[frame % SPINNER_FRAME_COUNT],
           SPINNER_RESET,
           message);
    fflush(stdout);
}

static void inline_spinner_clear(void) {
    printf("\r\033[K");
    fflush(stdout);
}

// Pulse indicator - for very short operations
static void pulse_indicator(const char *message) {
    for (int i = 0; i < 3; i++) {
        printf("\r\033[K%s●%s %s", SPINNER_CYAN, SPINNER_RESET, message);
        fflush(stdout);
        usleep(100000);  // 100ms
        printf("\r\033[K%s○%s %s", SPINNER_CYAN, SPINNER_RESET, message);
        fflush(stdout);
        usleep(100000);
    }
    printf("\r\033[K");
    fflush(stdout);
}

#endif // INDICATORS_H
