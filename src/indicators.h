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
#include "fallback_colors.h"

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

// Color codes - use centralized fallback system
#define SPINNER_CYAN ANSI_FALLBACK_CYAN
#define SPINNER_YELLOW ANSI_FALLBACK_YELLOW
#define SPINNER_GREEN ANSI_FALLBACK_GREEN
#define SPINNER_BLUE ANSI_FALLBACK_BLUE
#define SPINNER_RESET ANSI_RESET

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
            printf("%s✗%s %s\n", ANSI_FALLBACK_ERROR, SPINNER_RESET, final_message);
        }
    }

    fflush(stdout);

    pthread_mutex_destroy(&spinner->lock);
    free(spinner->message);
    free(spinner);
}

#endif // INDICATORS_H
