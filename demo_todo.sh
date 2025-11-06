#!/bin/bash
# Demo script to show TODO list rendering with different themes

echo "=========================================="
echo "TODO List Rendering Demo"
echo "=========================================="
echo ""

# Create a simple test program that uses the TODO list
cat > /tmp/test_todo_demo.c << 'EOF'
#include "src/todo.h"
#include "src/logger.h"
#include <stdio.h>
#include <stdlib.h>

// Color scheme needs to be initialized
#define COLORSCHEME_EXTERN
#include "src/colorscheme.h"
#include "src/fallback_colors.h"

int main(int argc, char *argv[]) {
    // Initialize colorscheme if theme provided
    const char *theme = getenv("CLAUDE_C_THEME");
    if (theme) {
        init_colorscheme(theme);
    }
    
    // Create and populate TODO list
    TodoList list = {0};
    todo_init(&list);
    
    todo_add(&list, "Build the project", "Building the project", TODO_COMPLETED);
    todo_add(&list, "Run unit tests", "Running unit tests", TODO_IN_PROGRESS);
    todo_add(&list, "Fix failing tests", "Fixing failing tests", TODO_PENDING);
    todo_add(&list, "Update documentation", "Updating documentation", TODO_PENDING);
    todo_add(&list, "Create release", "Creating release", TODO_PENDING);
    
    // Render the list
    char *rendered = todo_render_to_string(&list);
    if (rendered) {
        printf("%s\n", rendered);
        free(rendered);
    }
    
    // Cleanup
    todo_free(&list);
    
    return 0;
}
EOF

# Compile the demo
echo "Compiling demo..."
cc -o /tmp/test_todo_demo /tmp/test_todo_demo.c build/todo.o build/logger.o -I. -I/opt/homebrew/include -L/opt/homebrew/lib -lcjson 2>/dev/null

if [ $? -ne 0 ]; then
    echo "Failed to compile demo. Try running 'make' first."
    exit 1
fi

echo ""
echo "=== Default (No Theme) ==="
/tmp/test_todo_demo

echo ""
echo "=== Dracula Theme ==="
CLAUDE_C_THEME="dracula" /tmp/test_todo_demo

echo ""
echo "=== Gruvbox Dark Theme ==="
CLAUDE_C_THEME="gruvbox-dark" /tmp/test_todo_demo

echo ""
echo "=== Solarized Dark Theme ==="
CLAUDE_C_THEME="solarized-dark" /tmp/test_todo_demo

# Cleanup
rm -f /tmp/test_todo_demo /tmp/test_todo_demo.c

echo ""
echo "=========================================="
echo "Demo complete!"
echo "=========================================="
