#include "status.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <locale.h>
#include <wctype.h>
#include <wchar.h>

// Zero-width markers
#define STATUS_START_CHAR "\xE2\x80\x8B"  // U+200B ZERO WIDTH SPACE
#define STATUS_END_CHAR   "\xE2\x80\x8C"  // U+200C ZERO WIDTH NON-JOINER

// Stored last status (with markers)
static char *last_status_marked = NULL;
// Cached width of last status content
static int last_width = 0;

int status_init(void) {
    // Ensure locale is set for wcwidth
    setlocale(LC_CTYPE, "");
    return 0;
}

void status_cleanup(void) {
    free(last_status_marked);
    last_status_marked = NULL;
    last_width = 0;
}

// Internal: compute display width of a UTF-8 string (skip markers)
static int compute_width(const char *s) {
    if (!s) return 0;
    int width = 0;
    mbstate_t state;
    memset(&state, 0, sizeof(state));
    const char *p = s;
    while (*p) {
        wchar_t wc;
        size_t len = mbrtowc(&wc, p, MB_CUR_MAX, &state);
        if (len == (size_t)-1 || len == (size_t)-2) {
            // invalid sequence: skip one byte
            p++;
            memset(&state, 0, sizeof(state));
            continue;
        }
        // Skip marker codepoints
        if (wc == 0x200B || wc == 0x200C) {
            p += len;
            continue;
        }
        int w = wcwidth(wc);
        if (w > 0) width += w;
        p += len;
    }
    return width;
}

void draw_status(const char *status) {
    // Compose marked string
    free(last_status_marked);
    size_t base_len = status ? strlen(status) : 0;
    size_t tot_len = strlen(STATUS_START_CHAR) + base_len + strlen(STATUS_END_CHAR) + 1;
    last_status_marked = malloc(tot_len);
    if (!last_status_marked) return;
    // wrap markers
    strcpy(last_status_marked, STATUS_START_CHAR);
    if (status) strcat(last_status_marked, status);
    strcat(last_status_marked, STATUS_END_CHAR);

    // Print directly (caller should have moved to status line or handles spinner)
    printf("%s", last_status_marked);
    fflush(stdout);

    // Cache width
    last_width = compute_width(last_status_marked);
}

void clear_status(void) {
    if (!last_status_marked) return;
    // Move cursor to beginning of status: carriage return
    printf("\r");
    // erase exact number of cols
    if (last_width > 0) {
        printf("\x1b[%dX", last_width);
    }
    fflush(stdout);
    // free stored
    free(last_status_marked);
    last_status_marked = NULL;
    last_width = 0;
}

const char *status_last(void) {
    return last_status_marked;
}

int status_last_width(void) {
    return last_width;
}
