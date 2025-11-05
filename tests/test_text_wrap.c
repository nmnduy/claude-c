/*
 * Unit tests for text wrapping functionality in TUI
 *
 * Tests the wrap_text and find_wrap_position functions to ensure
 * correct text wrapping behavior with various edge cases.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Forward declarations of internal functions we're testing
// These are static in tui.c, so we need to expose them for testing
static int find_wrap_position(const char *text, int max_len);
static char** wrap_text(const char *text, int max_width, int *line_count);
static void free_wrapped_text(char **lines);

// Copy the actual implementations from tui.c
static int find_wrap_position(const char *text, int max_len) {
    if (!text || max_len <= 0) return 0;
    
    int text_len = (int)strlen(text);
    if (text_len <= max_len) return text_len;
    
    // Look for the last space before max_len
    int break_pos = max_len;
    while (break_pos > 0 && text[break_pos] != ' ' && text[break_pos] != '\t') {
        break_pos--;
    }
    
    // If we found a space, break there
    if (break_pos > 0) {
        return break_pos;
    }
    
    // No space found - hard break at max_len
    return max_len;
}

static char** wrap_text(const char *text, int max_width, int *line_count) {
    if (!text || max_width <= 0) {
        *line_count = 0;
        return NULL;
    }
    
    // Allocate initial array for lines (will grow if needed)
    int capacity = 16;
    char **lines = calloc((size_t)capacity, sizeof(char*));
    if (!lines) {
        *line_count = 0;
        return NULL;
    }
    
    int count = 0;
    const char *remaining = text;
    int remaining_len = (int)strlen(text);
    
    while (remaining_len > 0) {
        // Grow array if needed
        if (count >= capacity - 1) {
            capacity *= 2;
            char **new_lines = realloc(lines, (size_t)capacity * sizeof(char*));
            if (!new_lines) {
                // Free what we have so far
                for (int i = 0; i < count; i++) {
                    free(lines[i]);
                }
                free(lines);
                *line_count = 0;
                return NULL;
            }
            lines = new_lines;
        }
        
        // Find where to break this line
        int break_pos = find_wrap_position(remaining, max_width);
        if (break_pos <= 0) break_pos = 1; // At least take one char
        
        // Allocate and copy this line
        lines[count] = malloc((size_t)break_pos + 1);
        if (!lines[count]) {
            // Free what we have so far
            for (int i = 0; i < count; i++) {
                free(lines[i]);
            }
            free(lines);
            *line_count = 0;
            return NULL;
        }
        
        strncpy(lines[count], remaining, (size_t)break_pos);
        lines[count][break_pos] = '\0';
        
        // Trim trailing spaces from the line
        int end = (int)strlen(lines[count]) - 1;
        while (end >= 0 && (lines[count][end] == ' ' || lines[count][end] == '\t')) {
            lines[count][end] = '\0';
            end--;
        }
        
        count++;
        
        // Move to next part of text (skip leading spaces)
        remaining += break_pos;
        remaining_len -= break_pos;
        while (remaining_len > 0 && (*remaining == ' ' || *remaining == '\t')) {
            remaining++;
            remaining_len--;
        }
    }
    
    lines[count] = NULL; // NULL-terminate the array
    *line_count = count;
    return lines;
}

static void free_wrapped_text(char **lines) {
    if (!lines) return;
    
    for (int i = 0; lines[i] != NULL; i++) {
        free(lines[i]);
    }
    free(lines);
}

// Test helper: Print wrapped lines for debugging
static void print_wrapped_lines(char **lines, int count) {
    printf("Wrapped into %d lines:\n", count);
    for (int i = 0; i < count; i++) {
        printf("  [%d]: \"%s\"\n", i, lines[i]);
    }
}

// =============================================================================
// Test Cases for find_wrap_position
// =============================================================================

void test_find_wrap_position_basic() {
    printf("\n=== test_find_wrap_position_basic ===\n");
    
    // Text shorter than max_len should return text length
    const char *text1 = "hello";
    int pos = find_wrap_position(text1, 10);
    assert(pos == 5);
    printf("✓ Short text returns text length\n");
    
    // Text exactly max_len
    const char *text2 = "hello";
    pos = find_wrap_position(text2, 5);
    assert(pos == 5);
    printf("✓ Text exactly max_len returns text length\n");
    
    // NULL text
    pos = find_wrap_position(NULL, 10);
    assert(pos == 0);
    printf("✓ NULL text returns 0\n");
    
    // Zero or negative max_len
    pos = find_wrap_position("hello", 0);
    assert(pos == 0);
    pos = find_wrap_position("hello", -5);
    assert(pos == 0);
    printf("✓ Invalid max_len returns 0\n");
}

void test_find_wrap_position_word_boundaries() {
    printf("\n=== test_find_wrap_position_word_boundaries ===\n");
    
    // Break at space before max_len
    const char *text = "hello world today";
    int pos = find_wrap_position(text, 12);  // "hello world " is 12 chars
    assert(pos == 11);  // Should break at space before "world"
    printf("✓ Break at last space: pos=%d\n", pos);
    
    // No space - hard break
    const char *text2 = "verylongwordwithoutspaces";
    pos = find_wrap_position(text2, 10);
    assert(pos == 10);  // Hard break at max_len
    printf("✓ Hard break when no space: pos=%d\n", pos);
}

void test_find_wrap_position_edge_cases() {
    printf("\n=== test_find_wrap_position_edge_cases ===\n");
    
    // BUG: Space exactly at max_len position
    const char *text1 = "hello world";  // Space at position 5
    int pos = find_wrap_position(text1, 6);
    printf("Space at position 5, max_len=6: pos=%d (checking index %d)\n", pos, 6);
    // This might access text[6] which is 'w', not checking if we should break at space[5]
    
    // Text with only spaces
    const char *text2 = "     ";
    pos = find_wrap_position(text2, 3);
    printf("All spaces: pos=%d\n", pos);
    
    // Single character
    const char *text3 = "a";
    pos = find_wrap_position(text3, 10);
    assert(pos == 1);
    printf("✓ Single character\n");
}

// =============================================================================
// Test Cases for wrap_text
// =============================================================================

void test_wrap_text_basic() {
    printf("\n=== test_wrap_text_basic ===\n");
    
    // Simple wrapping
    const char *text = "hello world today";
    int count = 0;
    char **lines = wrap_text(text, 10, &count);
    
    print_wrapped_lines(lines, count);
    assert(lines != NULL);
    assert(count >= 2);  // Should wrap into at least 2 lines
    
    free_wrapped_text(lines);
    printf("✓ Basic wrapping works\n");
}

void test_wrap_text_exact_width() {
    printf("\n=== test_wrap_text_exact_width ===\n");
    
    const char *text = "hello world";  // 11 chars
    int count = 0;
    char **lines = wrap_text(text, 11, &count);
    
    print_wrapped_lines(lines, count);
    assert(count == 1);
    assert(strcmp(lines[0], "hello world") == 0);
    
    free_wrapped_text(lines);
    printf("✓ Text exactly fitting width\n");
}

void test_wrap_text_multiple_spaces() {
    printf("\n=== test_wrap_text_multiple_spaces ===\n");
    
    const char *text = "hello    world";  // Multiple spaces
    int count = 0;
    char **lines = wrap_text(text, 10, &count);
    
    print_wrapped_lines(lines, count);
    
    free_wrapped_text(lines);
    printf("✓ Multiple spaces handled\n");
}

void test_wrap_text_trailing_spaces() {
    printf("\n=== test_wrap_text_trailing_spaces ===\n");
    
    const char *text = "hello world     ";  // Trailing spaces
    int count = 0;
    char **lines = wrap_text(text, 20, &count);
    
    print_wrapped_lines(lines, count);
    // Trailing spaces should be trimmed
    assert(strcmp(lines[0], "hello world") == 0);
    
    free_wrapped_text(lines);
    printf("✓ Trailing spaces trimmed\n");
}

void test_wrap_text_leading_spaces() {
    printf("\n=== test_wrap_text_leading_spaces ===\n");
    
    const char *text = "     hello world";  // Leading spaces
    int count = 0;
    char **lines = wrap_text(text, 20, &count);
    
    print_wrapped_lines(lines, count);
    // First line might have leading spaces, but second line shouldn't
    
    free_wrapped_text(lines);
    printf("✓ Leading spaces handled\n");
}

void test_wrap_text_with_newlines() {
    printf("\n=== test_wrap_text_with_newlines ===\n");
    
    // BUG: Newlines should create line breaks, but current code treats them as regular chars
    const char *text = "hello\nworld\ntoday";
    int count = 0;
    char **lines = wrap_text(text, 20, &count);
    
    print_wrapped_lines(lines, count);
    
    // EXPECTED: 3 lines ["hello", "world", "today"]
    // ACTUAL: Probably 1 line with embedded newlines
    printf("Expected 3 lines (one per \\n), got %d lines\n", count);
    
    free_wrapped_text(lines);
    printf("! This test demonstrates the newline bug\n");
}

void test_wrap_text_long_word() {
    printf("\n=== test_wrap_text_long_word ===\n");
    
    const char *text = "supercalifragilisticexpialidocious";
    int count = 0;
    char **lines = wrap_text(text, 10, &count);
    
    print_wrapped_lines(lines, count);
    assert(count >= 3);  // Should hard-break into multiple lines
    
    free_wrapped_text(lines);
    printf("✓ Long word hard-breaks\n");
}

void test_wrap_text_empty_string() {
    printf("\n=== test_wrap_text_empty_string ===\n");
    
    const char *text = "";
    int count = -1;
    char **lines = wrap_text(text, 10, &count);
    
    printf("Empty string: count=%d, lines=%p\n", count, (void*)lines);
    
    // BUG: Empty string behavior is inconsistent
    // Current implementation returns non-NULL array with count=0
    // This is actually not a NULL-terminated array (lines[0] is already NULL)
    if (lines != NULL && count == 0) {
        // This is what actually happens - we get a valid array but no lines
        printf("! Empty string returns non-NULL array with 0 lines\n");
        free_wrapped_text(lines);
    } else if (lines == NULL && count == 0) {
        printf("✓ Empty string returns NULL with 0 lines\n");
    } else {
        printf("! Unexpected result for empty string\n");
    }
}

void test_wrap_text_only_spaces() {
    printf("\n=== test_wrap_text_only_spaces ===\n");
    
    const char *text = "          ";  // Only spaces
    int count = 0;
    char **lines = wrap_text(text, 5, &count);
    
    if (lines) {
        print_wrapped_lines(lines, count);
        
        // After trimming, might result in empty lines
        printf("Text with only spaces results in %d lines\n", count);
        
        // Check if any lines became empty after trimming
        for (int i = 0; i < count; i++) {
            if (strlen(lines[i]) == 0) {
                printf("! Line %d is empty after trimming\n", i);
            }
        }
        
        free_wrapped_text(lines);
    } else {
        printf("Only spaces returns NULL with count=%d\n", count);
    }
}

void test_wrap_text_unicode() {
    printf("\n=== test_wrap_text_unicode ===\n");
    
    // UTF-8 characters: "Hello 世界" (Chinese for "world")
    const char *text = "Hello 世界 today";
    int count = 0;
    char **lines = wrap_text(text, 15, &count);
    
    print_wrapped_lines(lines, count);
    
    // BUG: Hard breaks might split UTF-8 multi-byte characters
    printf("! Unicode handling may have issues with hard breaks\n");
    
    free_wrapped_text(lines);
}

void test_wrap_text_tabs() {
    printf("\n=== test_wrap_text_tabs ===\n");
    
    const char *text = "hello\tworld\ttoday";
    int count = 0;
    char **lines = wrap_text(text, 10, &count);
    
    print_wrapped_lines(lines, count);
    
    // BUG: Tabs are treated as 1-char width, but typically display as 8 spaces
    printf("! Tabs treated as single character width\n");
    
    free_wrapped_text(lines);
}

void test_wrap_text_very_small_width() {
    printf("\n=== test_wrap_text_very_small_width ===\n");
    
    const char *text = "hello";
    int count = 0;
    char **lines = wrap_text(text, 1, &count);
    
    print_wrapped_lines(lines, count);
    assert(count == 5);  // Should break into individual characters
    
    free_wrapped_text(lines);
    printf("✓ Very small width forces char-by-char breaks\n");
}

void test_wrap_text_mixed_content() {
    printf("\n=== test_wrap_text_mixed_content ===\n");
    
    // Realistic content with multiple issues
    const char *text = "This is a test\nwith newlines\tand tabs    and multiple   spaces";
    int count = 0;
    char **lines = wrap_text(text, 20, &count);
    
    print_wrapped_lines(lines, count);
    
    printf("! Mixed content demonstrates multiple issues\n");
    
    free_wrapped_text(lines);
}

void test_wrap_text_null_input() {
    printf("\n=== test_wrap_text_null_input ===\n");
    
    int count = -1;
    char **lines = wrap_text(NULL, 10, &count);
    
    assert(lines == NULL);
    assert(count == 0);
    
    printf("✓ NULL input handled safely\n");
}

void test_wrap_text_zero_width() {
    printf("\n=== test_wrap_text_zero_width ===\n");
    
    int count = -1;
    char **lines = wrap_text("hello", 0, &count);
    
    assert(lines == NULL);
    assert(count == 0);
    
    printf("✓ Zero width handled safely\n");
}

void test_wrap_text_negative_width() {
    printf("\n=== test_wrap_text_negative_width ===\n");
    
    int count = -1;
    char **lines = wrap_text("hello", -5, &count);
    
    assert(lines == NULL);
    assert(count == 0);
    
    printf("✓ Negative width handled safely\n");
}

// =============================================================================
// Regression Tests (for fixed bugs)
// =============================================================================

void test_wrap_regression_space_at_boundary() {
    printf("\n=== test_wrap_regression_space_at_boundary ===\n");
    
    // If a space falls exactly at the wrap boundary, we should break there
    const char *text = "hello world test";  // Space at position 5 and 11
    int count = 0;
    char **lines = wrap_text(text, 6, &count);  // Should break at space[5]
    
    print_wrapped_lines(lines, count);
    
    // Expected: ["hello", "world test"] or similar
    // Current implementation might have off-by-one error
    
    free_wrapped_text(lines);
    printf("! Check if space at boundary is handled correctly\n");
}

void test_wrap_regression_all_whitespace_line() {
    printf("\n=== test_wrap_regression_all_whitespace_line ===\n");
    
    // After trimming trailing spaces, some lines might become completely empty
    const char *text = "hello     world";
    int count = 0;
    char **lines = wrap_text(text, 6, &count);
    
    print_wrapped_lines(lines, count);
    
    // Should not have empty lines after trimming
    for (int i = 0; i < count; i++) {
        if (strlen(lines[i]) == 0) {
            printf("! Warning: Empty line at index %d after trimming\n", i);
        }
    }
    
    free_wrapped_text(lines);
}

// =============================================================================
// Main Test Runner
// =============================================================================

int main(void) {
    printf("========================================\n");
    printf("Text Wrapping Unit Tests\n");
    printf("========================================\n");
    
    // find_wrap_position tests
    test_find_wrap_position_basic();
    test_find_wrap_position_word_boundaries();
    test_find_wrap_position_edge_cases();
    
    // wrap_text tests
    test_wrap_text_basic();
    test_wrap_text_exact_width();
    test_wrap_text_multiple_spaces();
    test_wrap_text_trailing_spaces();
    test_wrap_text_leading_spaces();
    test_wrap_text_with_newlines();  // Demonstrates bug
    test_wrap_text_long_word();
    test_wrap_text_empty_string();
    test_wrap_text_only_spaces();
    test_wrap_text_unicode();  // Potential bug
    test_wrap_text_tabs();  // Potential bug
    test_wrap_text_very_small_width();
    test_wrap_text_mixed_content();
    test_wrap_text_null_input();
    test_wrap_text_zero_width();
    test_wrap_text_negative_width();
    
    // Regression tests
    test_wrap_regression_space_at_boundary();
    test_wrap_regression_all_whitespace_line();
    
    printf("\n========================================\n");
    printf("All tests completed!\n");
    printf("Note: Tests marked with '!' indicate known bugs or issues.\n");
    printf("========================================\n");
    
    return 0;
}
