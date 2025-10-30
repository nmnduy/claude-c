#ifndef UI_STATUS_H
#define UI_STATUS_H

#include <stddef.h>

// Initialize status module (sets locale if needed)
int status_init(void);
// Clean up status module
void status_cleanup(void);

// Draw status text on the terminal, wrapping with invisible markers
void draw_status(const char *status);
// Clear the previously drawn status, erasing exact columns
void clear_status(void);

// Test helper: get last status with markers (or NULL)
const char *status_last(void);
// Test helper: compute width of last status content
int status_last_width(void);

#endif // UI_STATUS_H
