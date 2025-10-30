#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ui/status.h"

int main(void) {
    assert(status_init() == 0);
    assert(status_last() == NULL);
    assert(status_last_width() == 0);

    draw_status("hello");
    const char *marked = status_last();
    if (!marked) return 1;
    assert(strstr(marked, "\xE2\x80\x8Bhello\xE2\x80\x8C") == marked);
    assert(status_last_width() == 5);

    clear_status();
    draw_status("世界");
    assert(status_last_width() == 4);

    clear_status();
    assert(status_last() == NULL);
    assert(status_last_width() == 0);

    status_cleanup();
    printf("Status tests passed\n");
    return 0;
}
