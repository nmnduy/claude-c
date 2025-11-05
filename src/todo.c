/*
 * TODO List Implementation
 */

#include "todo.h"
#include "fallback_colors.h"
#define COLORSCHEME_EXTERN
#include "colorscheme.h"
#include "logger.h"
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

char* todo_render_to_string(const TodoList *list) {
    if (!list || list->count == 0) {
        return NULL;  // No todos to display
    }

    // Calculate approximate buffer size needed
    size_t buffer_size = 256;  // Base size for intro text
    for (size_t i = 0; i < list->count; i++) {
        buffer_size += strlen(list->items[i].content) + strlen(list->items[i].active_form) + 50;
    }

    char *result = malloc(buffer_size);
    if (!result) {
        LOG_ERROR("Failed to allocate memory for todo render string");
        return NULL;
    }

    size_t offset = 0;

    offset += (size_t)snprintf(result + offset, buffer_size - offset,
                               "Here are the current tasks:\n");

    // Render each item with appropriate indicator
    for (size_t i = 0; i < list->count; i++) {
        const TodoItem *item = &list->items[i];

        switch (item->status) {
            default:
                LOG_WARN("Unknown TODO status: %d", (int)item->status);
                offset += (size_t)snprintf(result + offset, buffer_size - offset,
                                 "• %s\n", item->content);
                break;
            case TODO_COMPLETED:
                offset += (size_t)snprintf(result + offset, buffer_size - offset,
                                 "✓ %s\n", item->content);
                break;

            case TODO_IN_PROGRESS:
                offset += (size_t)snprintf(result + offset, buffer_size - offset,
                                 "⋯ %s\n", item->active_form);
                break;

            case TODO_PENDING:
                offset += (size_t)snprintf(result + offset, buffer_size - offset,
                                 "○ %s\n", item->content);
                break;
        }
    }

    // Remove trailing newline if present
    if (offset > 0 && offset <= buffer_size && result[offset - 1] == '\n') {
        result[offset - 1] = '\0';
    }

    return result;
}

void todo_render(const TodoList *list) {
    if (!list || list->count == 0) {
        return;  // No todos to display
    }

    char *text = todo_render_to_string(list);
    if (!text) {
        return;
    }

    printf("%s\n", text);
    fflush(stdout);
    free(text);
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
