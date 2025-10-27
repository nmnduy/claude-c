/*
 * Unit Tests for Line Editor
 *
 * Tests the line editor functionality including:
 * - Terminal wrapping calculations
 * - UTF-8 character parsing
 * - Command history management
 * - Cursor positioning at various positions
 * - Edge cases (empty input, cursor at end, etc.)
 *
 * Compilation: make test-lineedit
 * Usage: ./build/test_lineedit
 */

#define _POSIX_C_SOURCE 200809L
#define TEST_BUILD 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/lineedit.h"

// Test framework colors
#define COLOR_RESET "\033[0m"
#define COLOR_GREEN "\033[32m"
#define COLOR_RED "\033[31m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_CYAN "\033[36m"

// Test counters
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

// Forward declarations from lineedit.c
extern void calculate_cursor_position(
    const char *buffer,
    int buffer_len,
    int cursor_pos,
    int prompt_len,
    int term_width,
    int *out_cursor_line,
    int *out_cursor_col,
    int *out_total_lines
);

// UTF-8 helper functions (internal, exposed for testing)
extern int utf8_char_length(unsigned char first_byte);
extern int is_utf8_continuation(unsigned char byte);

// ============================================================================
// Test Utilities
// ============================================================================

static void assert_true(const char *test_name, int condition) {
    tests_run++;
    if (condition) {
        tests_passed++;
        printf("%s✓%s %s\n", COLOR_GREEN, COLOR_RESET, test_name);
    } else {
        tests_failed++;
        printf("%s✗%s %s\n", COLOR_RED, COLOR_RESET, test_name);
    }
}

static void assert_equals(const char *test_name, int expected, int actual) {
    tests_run++;
    if (expected == actual) {
        tests_passed++;
        printf("%s✓%s %s\n", COLOR_GREEN, COLOR_RESET, test_name);
    } else {
        tests_failed++;
        printf("%s✗%s %s\n", COLOR_RED, COLOR_RESET, test_name);
        printf("%s  Expected: %d, Actual: %d%s\n",
               COLOR_YELLOW, expected, actual, COLOR_RESET);
    }
}

static void assert_position(const char *test_name,
                           const char *buffer,
                           int cursor_pos,
                           int prompt_len,
                           int term_width,
                           int expected_line,
                           int expected_col,
                           int expected_total_lines) {
    tests_run++;

    int cursor_line, cursor_col, total_lines;
    calculate_cursor_position(buffer, strlen(buffer), cursor_pos, prompt_len, term_width,
                             &cursor_line, &cursor_col, &total_lines);

    int matches = (cursor_line == expected_line &&
                  cursor_col == expected_col &&
                  total_lines == expected_total_lines);

    if (matches) {
        tests_passed++;
        printf("%s✓%s %s\n", COLOR_GREEN, COLOR_RESET, test_name);
    } else {
        tests_failed++;
        printf("%s✗%s %s\n", COLOR_RED, COLOR_RESET, test_name);
        printf("%s  Expected: line=%d col=%d total=%d%s\n",
               COLOR_YELLOW, expected_line, expected_col, expected_total_lines, COLOR_RESET);
        printf("%s  Actual:   line=%d col=%d total=%d%s\n",
               COLOR_YELLOW, cursor_line, cursor_col, total_lines, COLOR_RESET);
    }
}

// ============================================================================
// Test Cases
// ============================================================================

static void test_simple_no_wrapping() {
    printf("\n%s[Test: Simple Input - No Wrapping]%s\n", COLOR_CYAN, COLOR_RESET);

    // Prompt: "> " (2 chars), buffer: "hello", term width: 80
    // No wrapping should occur
    assert_position("Empty buffer",
                   "", 0, 2, 80,
                   0, 2, 0);  // Cursor at prompt end

    assert_position("Short input - cursor at start",
                   "hello", 0, 2, 80,
                   0, 2, 0);  // Cursor at prompt end

    assert_position("Short input - cursor in middle",
                   "hello", 3, 2, 80,
                   0, 5, 0);  // Line 0, col 2+3=5

    assert_position("Short input - cursor at end",
                   "hello", 5, 2, 80,
                   0, 7, 0);  // Line 0, col 2+5=7
}

static void test_wrapping_at_edge() {
    printf("\n%s[Test: Wrapping at Terminal Edge]%s\n", COLOR_CYAN, COLOR_RESET);

    // Prompt: "> " (2 chars), term width: 20
    // Text: "123456789012345678" (18 chars)
    // Total on line 1: 2 (prompt) + 18 (text) = 20 chars exactly
    const char *text = "123456789012345678";

    assert_position("Cursor at start - fits exactly",
                   text, 0, 2, 20,
                   0, 2, 0);  // Still on first line

    assert_position("Cursor in middle - fits exactly",
                   text, 9, 2, 20,
                   0, 11, 0);  // Line 0, col 2+9=11

    assert_position("Cursor at end - fits exactly",
                   text, 18, 2, 20,
                   0, 20, 0);  // Line 0, col 2+18=20 (but displayed as col 20)
}

static void test_wrapping_one_overflow() {
    printf("\n%s[Test: Wrapping - One Character Overflow]%s\n", COLOR_CYAN, COLOR_RESET);

    // Prompt: "> " (2 chars), term width: 20
    // Text: "1234567890123456789" (19 chars)
    // Total: 2 + 19 = 21 chars, wraps to 2 lines
    const char *text = "1234567890123456789";

    assert_position("One char overflow - cursor at start",
                   text, 0, 2, 20,
                   0, 2, 1);  // Line 0, total spans 2 lines (0-1)

    assert_position("One char overflow - cursor before wrap",
                   text, 17, 2, 20,
                   0, 19, 1);  // Line 0, col 19

    assert_position("One char overflow - cursor at wrap point",
                   text, 18, 2, 20,
                   0, 20, 1);  // Line 0, col 20 (wraps after this)

    assert_position("One char overflow - cursor after wrap",
                   text, 19, 2, 20,
                   1, 1, 1);  // Line 1, col 1
}

static void test_wrapping_multiple_lines() {
    printf("\n%s[Test: Wrapping - Multiple Lines]%s\n", COLOR_CYAN, COLOR_RESET);

    // Prompt: "> " (2 chars), term width: 10
    // Text: "12345678901234567890" (20 chars)
    // Line 0: "> 12345678" (10 chars)
    // Line 1: "9012345678" (10 chars)
    // Line 2: "90" (2 chars)
    const char *text = "12345678901234567890";

    assert_position("Multi-wrap - cursor at start",
                   text, 0, 2, 10,
                   0, 2, 2);  // Line 0, spans 3 lines (0-2)

    assert_position("Multi-wrap - cursor at end of line 0",
                   text, 8, 2, 10,
                   0, 10, 2);  // Line 0, col 10

    assert_position("Multi-wrap - cursor at start of line 1",
                   text, 9, 2, 10,
                   1, 1, 2);  // Line 1, col 1

    assert_position("Multi-wrap - cursor in middle of line 1",
                   text, 13, 2, 10,
                   1, 5, 2);  // Line 1, col 5

    assert_position("Multi-wrap - cursor at end of line 1",
                   text, 18, 2, 10,
                   1, 10, 2);  // Line 1, col 10

    assert_position("Multi-wrap - cursor on line 2",
                   text, 19, 2, 10,
                   2, 1, 2);  // Line 2, col 1

    assert_position("Multi-wrap - cursor at end",
                   text, 20, 2, 10,
                   2, 2, 2);  // Line 2, col 2
}

static void test_manual_newlines() {
    printf("\n%s[Test: Manual Newlines]%s\n", COLOR_CYAN, COLOR_RESET);

    // Prompt: "> " (2 chars), term width: 80
    // Text with manual newlines
    const char *text = "hello\nworld";

    assert_position("Newline - cursor at start",
                   text, 0, 2, 80,
                   0, 2, 1);  // Line 0, spans 2 lines

    assert_position("Newline - cursor before \\n",
                   text, 5, 2, 80,
                   0, 7, 1);  // Line 0, col 7

    assert_position("Newline - cursor after \\n",
                   text, 6, 2, 80,
                   1, 0, 1);  // Line 1, col 0 (no prompt on line 1)

    assert_position("Newline - cursor at end",
                   text, 11, 2, 80,
                   1, 5, 1);  // Line 1, col 5
}

static void test_manual_newlines_with_wrapping() {
    printf("\n%s[Test: Manual Newlines + Wrapping]%s\n", COLOR_CYAN, COLOR_RESET);

    // Prompt: "> " (2 chars), term width: 10
    // Text: "12345678\n12345678" (8 chars + newline + 8 chars)
    // Line 0: "> 12345678" (10 chars, fits exactly)
    // Line 1: "12345678" (8 chars, no prompt)
    const char *text = "12345678\n12345678";

    assert_position("Newline+wrap - before newline",
                   text, 7, 2, 10,
                   0, 9, 1);  // Line 0, col 9

    assert_position("Newline+wrap - at newline",
                   text, 8, 2, 10,
                   0, 10, 1);  // Line 0, col 10

    assert_position("Newline+wrap - after newline",
                   text, 9, 2, 10,
                   1, 0, 1);  // Line 1, col 0

    // Now test with wrapping on second line
    // Prompt: "> " (2 chars), term width: 10
    // Text: "12345678\n123456789012" (wraps on line 2)
    // Line 0: "> 12345678" (10 chars)
    // Line 1: "1234567890" (10 chars)
    // Line 2: "12" (2 chars)
    const char *text2 = "12345678\n123456789012";

    assert_position("Newline+wrap - line 1 wraps",
                   text2, 17, 2, 10,
                   1, 8, 2);  // Line 1, col 8

    assert_position("Newline+wrap - wrapped to line 2",
                   text2, 19, 2, 10,
                   1, 10, 2);  // Line 1, col 10

    assert_position("Newline+wrap - on line 2",
                   text2, 20, 2, 10,
                   2, 1, 2);  // Line 2, col 1
}

static void test_edge_cases() {
    printf("\n%s[Test: Edge Cases]%s\n", COLOR_CYAN, COLOR_RESET);

    // Zero width terminal (fallback should handle)
    assert_position("Zero term width - empty",
                   "", 0, 2, 1,
                   0, 2, 0);

    // Very long prompt
    assert_position("Long prompt - short text",
                   "hi", 0, 50, 80,
                   0, 50, 0);  // Cursor at prompt end

    assert_position("Long prompt - short text at end",
                   "hi", 2, 50, 80,
                   0, 52, 0);  // Cursor at 50+2

    // Prompt + text exceeds width
    // Prompt: 50 chars, text: "hello" (5 chars), cursor at end
    // Line 0: prompt(50) + "he" = 52 chars
    // Line 1: "llo" (3 chars), cursor at end
    assert_position("Long prompt causes wrap",
                   "hello", 5, 50, 52,
                   1, 3, 1);  // Line 1, col 3, spans 2 lines (0-1)

    // Let's test this more carefully
    // Prompt: 50 chars, text: "12345", term width: 52
    // Line 0: prompt(50) + "12" = 52 chars
    // Line 1: "345" (3 chars)
    assert_position("Long prompt causes wrap - corrected",
                   "12345", 2, 50, 52,
                   0, 52, 1);  // At end of line 0

    assert_position("Long prompt causes wrap - on line 1",
                   "12345", 3, 50, 52,
                   1, 1, 1);  // Line 1, col 1
}

static void test_cursor_at_boundaries() {
    printf("\n%s[Test: Cursor at Wrap Boundaries]%s\n", COLOR_CYAN, COLOR_RESET);

    // Test cursor exactly at wrap boundary
    // Prompt: 0 (no prompt), term width: 10
    // Text: "1234567890X" (11 chars)
    // Line 0: "1234567890" (10 chars exactly)
    // Line 1: "X" (1 char)

    assert_position("Cursor at char 9 (before boundary)",
                   "1234567890X", 9, 0, 10,
                   0, 9, 1);  // Line 0, col 9

    assert_position("Cursor at char 10 (at boundary)",
                   "1234567890X", 10, 0, 10,
                   0, 10, 1);  // Line 0, col 10 (wraps after)

    assert_position("Cursor at char 11 (after boundary)",
                   "1234567890X", 11, 0, 10,
                   1, 1, 1);  // Line 1, col 1
}

// ============================================================================
// UTF-8 Tests
// ============================================================================

static void test_utf8_char_length() {
    printf("\n%s[Test: UTF-8 Character Length Detection]%s\n", COLOR_CYAN, COLOR_RESET);

    // ASCII (1 byte)
    assert_equals("ASCII 'A'", 1, utf8_char_length('A'));
    assert_equals("ASCII '0'", 1, utf8_char_length('0'));
    assert_equals("ASCII space", 1, utf8_char_length(' '));

    // 2-byte UTF-8 (110xxxxx)
    assert_equals("2-byte start (0xC0)", 2, utf8_char_length(0xC0));
    assert_equals("2-byte start (0xDF)", 2, utf8_char_length(0xDF));

    // 3-byte UTF-8 (1110xxxx)
    assert_equals("3-byte start (0xE0)", 3, utf8_char_length(0xE0));
    assert_equals("3-byte start (0xEF)", 3, utf8_char_length(0xEF));

    // 4-byte UTF-8 (11110xxx)
    assert_equals("4-byte start (0xF0)", 4, utf8_char_length(0xF0));
    assert_equals("4-byte start (0xF7)", 4, utf8_char_length(0xF7));

    // Invalid/continuation bytes (should return 1)
    assert_equals("Continuation byte (0x80)", 1, utf8_char_length(0x80));
    assert_equals("Continuation byte (0xBF)", 1, utf8_char_length(0xBF));
}

static void test_utf8_continuation() {
    printf("\n%s[Test: UTF-8 Continuation Byte Detection]%s\n", COLOR_CYAN, COLOR_RESET);

    // Valid continuation bytes (10xxxxxx pattern)
    assert_true("0x80 is continuation", is_utf8_continuation(0x80));
    assert_true("0xBF is continuation", is_utf8_continuation(0xBF));
    assert_true("0xA0 is continuation", is_utf8_continuation(0xA0));

    // Invalid continuation bytes
    assert_true("ASCII 'A' not continuation", !is_utf8_continuation('A'));
    assert_true("0xC0 not continuation", !is_utf8_continuation(0xC0));
    assert_true("0xE0 not continuation", !is_utf8_continuation(0xE0));
    assert_true("0xF0 not continuation", !is_utf8_continuation(0xF0));
}

// ============================================================================
// History Tests
// ============================================================================

static void test_history_basic() {
    printf("\n%s[Test: History Basic Operations]%s\n", COLOR_CYAN, COLOR_RESET);

    LineEditor ed;
    lineedit_init(&ed, NULL, NULL);

    // Initially empty
    assert_equals("History starts empty", 0, ed.history.count);
    assert_equals("History position is -1", -1, ed.history.position);

    // Manually add entries (simulating what lineedit_readline does)
    char *entries[] = {"first command", "second command", "third command"};
    for (int i = 0; i < 3; i++) {
        if (ed.history.count >= ed.history.capacity) {
            free(ed.history.entries[0]);
            memmove(&ed.history.entries[0], &ed.history.entries[1],
                    sizeof(char*) * (size_t)(ed.history.capacity - 1));
            ed.history.count--;
        }
        ed.history.entries[ed.history.count] = strdup(entries[i]);
        ed.history.count++;
    }

    assert_equals("History has 3 entries", 3, ed.history.count);
    assert_true("First entry correct",
                strcmp(ed.history.entries[0], "first command") == 0);
    assert_true("Second entry correct",
                strcmp(ed.history.entries[1], "second command") == 0);
    assert_true("Third entry correct",
                strcmp(ed.history.entries[2], "third command") == 0);

    lineedit_free(&ed);
}

static void test_history_capacity() {
    printf("\n%s[Test: History Capacity Limit]%s\n", COLOR_CYAN, COLOR_RESET);

    LineEditor ed;
    lineedit_init(&ed, NULL, NULL);

    int original_capacity = ed.history.capacity;

    // Add more than capacity entries
    for (int i = 0; i < original_capacity + 10; i++) {
        char entry[32];
        snprintf(entry, sizeof(entry), "command %d", i);

        // Simulate history_add logic
        if (ed.history.count >= ed.history.capacity) {
            free(ed.history.entries[0]);
            memmove(&ed.history.entries[0], &ed.history.entries[1],
                    sizeof(char*) * (size_t)(ed.history.capacity - 1));
            ed.history.count--;
        }
        ed.history.entries[ed.history.count] = strdup(entry);
        ed.history.count++;
    }

    assert_equals("History doesn't exceed capacity",
                  original_capacity, ed.history.count);

    // Check that oldest entries were removed
    char expected_first[32];
    snprintf(expected_first, sizeof(expected_first), "command %d", 10);
    assert_true("Oldest entries removed",
                strcmp(ed.history.entries[0], expected_first) == 0);

    lineedit_free(&ed);
}

static void test_history_navigation() {
    printf("\n%s[Test: History Navigation State]%s\n", COLOR_CYAN, COLOR_RESET);

    LineEditor ed;
    lineedit_init(&ed, NULL, NULL);

    // Add some entries
    char *entries[] = {"cmd1", "cmd2", "cmd3"};
    for (int i = 0; i < 3; i++) {
        ed.history.entries[ed.history.count] = strdup(entries[i]);
        ed.history.count++;
    }

    // Simulate Up arrow navigation
    assert_equals("Position starts at -1", -1, ed.history.position);

    // First Up press
    ed.history.position = ed.history.count;  // Set to end
    if (ed.history.position > 0) {
        ed.history.position--;
    }
    assert_equals("Position after first Up", 2, ed.history.position);

    // Second Up press
    if (ed.history.position > 0) {
        ed.history.position--;
    }
    assert_equals("Position after second Up", 1, ed.history.position);

    // Down press
    ed.history.position++;
    assert_equals("Position after Down", 2, ed.history.position);

    lineedit_free(&ed);
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("\n%s╔════════════════════════════════════════════╗%s\n",
           COLOR_CYAN, COLOR_RESET);
    printf("%s║   Line Editor Test Suite                   ║%s\n",
           COLOR_CYAN, COLOR_RESET);
    printf("%s╚════════════════════════════════════════════╝%s\n",
           COLOR_CYAN, COLOR_RESET);

    // UTF-8 tests
    test_utf8_char_length();
    test_utf8_continuation();

    // History tests
    test_history_basic();
    test_history_capacity();
    test_history_navigation();

    // Wrapping tests
    test_simple_no_wrapping();
    test_wrapping_at_edge();
    test_wrapping_one_overflow();
    test_wrapping_multiple_lines();
    test_manual_newlines();
    test_manual_newlines_with_wrapping();
    test_edge_cases();
    test_cursor_at_boundaries();

    // Print summary
    printf("\n%s╔════════════════════════════════════════════╗%s\n",
           COLOR_CYAN, COLOR_RESET);
    printf("%s║   Test Summary                             ║%s\n",
           COLOR_CYAN, COLOR_RESET);
    printf("%s╚════════════════════════════════════════════╝%s\n",
           COLOR_CYAN, COLOR_RESET);

    printf("\nTotal tests:  %d\n", tests_run);
    printf("%sPassed:       %d%s\n", COLOR_GREEN, tests_passed, COLOR_RESET);

    if (tests_failed > 0) {
        printf("%sFailed:       %d%s\n", COLOR_RED, tests_failed, COLOR_RESET);
        return 1;
    } else {
        printf("\n%s✓ All tests passed!%s\n\n", COLOR_GREEN, COLOR_RESET);
        return 0;
    }
}
