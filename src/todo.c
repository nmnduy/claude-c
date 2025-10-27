/*
 * TODO List Implementation
 */

#include "todo.h"
#include "fallback_colors.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define INITIAL_CAPACITY 10

// Helper: Duplicate a string
static char* str_dup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, str, len + 1);
    }
    return dup;
}

int todo_init(TodoList *list) {
    if (!list) return -1;

    list->items = malloc(INITIAL_CAPACITY * sizeof(TodoItem));
    if (!list->items) return -1;

    list->count = 0;
    list->capacity = INITIAL_CAPACITY;

    return 0;
}

void todo_free(TodoList *list) {
    if (!list) return;

    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].content);
        free(list->items[i].active_form);
    }
    free(list->items);

    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

int todo_add(TodoList *list, const char *content, const char *active_form, TodoStatus status) {
    if (!list || !content || !active_form) return -1;

    // Expand capacity if needed
    if (list->count >= list->capacity) {
        size_t new_capacity = list->capacity * 2;
        TodoItem *new_items = realloc(list->items, new_capacity * sizeof(TodoItem));
        if (!new_items) return -1;

        list->items = new_items;
        list->capacity = new_capacity;
    }

    // Add new item
    TodoItem *item = &list->items[list->count];
    item->content = str_dup(content);
    item->active_form = str_dup(active_form);
    item->status = status;

    if (!item->content || !item->active_form) {
        free(item->content);
        free(item->active_form);
        return -1;
    }

    list->count++;
    return 0;
}

int todo_update_status(TodoList *list, size_t index, TodoStatus status) {
    if (!list || index >= list->count) return -1;

    list->items[index].status = status;
    return 0;
}

int todo_update_by_content(TodoList *list, const char *content, TodoStatus status) {
    if (!list || !content) return -1;

    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].content, content) == 0) {
            list->items[i].status = status;
            return 0;
        }
    }

    return -1;  // Not found
}

int todo_remove(TodoList *list, size_t index) {
    if (!list || index >= list->count) return -1;

    // Free the item being removed
    free(list->items[index].content);
    free(list->items[index].active_form);

    // Shift remaining items down
    for (size_t i = index; i < list->count - 1; i++) {
        list->items[i] = list->items[i + 1];
    }

    list->count--;
    return 0;
}

void todo_clear(TodoList *list) {
    if (!list) return;

    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].content);
        free(list->items[i].active_form);
    }

    list->count = 0;
}

size_t todo_count_by_status(const TodoList *list, TodoStatus status) {
    if (!list) return 0;

    size_t count = 0;
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].status == status) {
            count++;
        }
    }
    return count;
}

void todo_render(const TodoList *list) {
    if (!list || list->count == 0) {
        return;  // No todos to display
    }

    // ANSI colors
    const char *green = ANSI_FALLBACK_GREEN;
    const char *yellow = ANSI_FALLBACK_YELLOW;
    const char *cyan = ANSI_FALLBACK_CYAN;
    const char *dim = ANSI_FALLBACK_DIM;
    const char *reset = ANSI_RESET;
    const char *bold = ANSI_FALLBACK_BOLD;

    // Print header
    printf("\n%s%s━━━ Tasks ━━━%s\n", bold, cyan, reset);

    // Render each item with appropriate indicator
    for (size_t i = 0; i < list->count; i++) {
        const TodoItem *item = &list->items[i];

        switch (item->status) {
            case TODO_COMPLETED:
                printf("%s✓ %s%s\n", green, item->content, reset);
                break;

            case TODO_IN_PROGRESS:
                printf("%s⋯ %s%s\n", yellow, item->active_form, reset);
                break;

            case TODO_PENDING:
                printf("%s○ %s%s\n", dim, item->content, reset);
                break;
        }
    }

    printf("%s━━━━━━━━━━━━%s\n\n", dim, reset);
    fflush(stdout);
}

// Simple parser for TODO updates from text
// Looks for common patterns like:
// - "marking X as completed"
// - "marking X as in_progress"
// - "adding todo: X"
int todo_parse_from_text(TodoList *list, const char *text) {
    if (!list || !text) return 0;

    int updates = 0;
    // TODO: Implement parsing logic
    // For now, this is a stub - we'll manually call todo functions
    // In the future, this could parse natural language from assistant responses

    return updates;
}
