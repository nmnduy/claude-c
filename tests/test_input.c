/*
 * Unit Tests for Input Handler
 *
 * Tests the input handler's functionality including:
 * - Word boundary detection
 * - Backward word movement (Alt+b)
 * - Forward word movement (Alt+f)
 * - Visible string length (ANSI-aware)
 * - Redraw input line (multiline support)
 *
 * Compilation: make test-input
 * Usage: ./build/test_input
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

// Forward declarations from claude.c
extern int is_word_boundary(char c);
extern int move_backward_word(const char *buffer, int cursor_pos);
extern int move_forward_word(const char *buffer, int cursor_pos, int buffer_len);
extern int visible_strlen(const char *str);

// Test utilities
#define ASSERT(test, message) do { \
    tests_run++; \
    if (test) { \
        tests_passed++; \
        printf("%s✓%s ", COLOR_GREEN, COLOR_RESET); \
    } else { \
        tests_failed++; \
        printf("%s✗%s ", COLOR_RED, COLOR_RESET); \
        printf("%sFAIL:%s %s\n", COLOR_RED, COLOR_RESET, message); \
    } \
} while (0)

#define ASSERT_EQ(actual, expected, message) do { \
    tests_run++; \
    if ((actual) == (expected)) { \
        tests_passed++; \
        printf("%s✓%s ", COLOR_GREEN, COLOR_RESET); \
    } else { \
        tests_failed++; \
        printf("%s✗%s ", COLOR_RED, COLOR_RESET); \
        printf("%sFAIL:%s %s (expected: %d, got: %d)\n", \
               COLOR_RED, COLOR_RESET, message, expected, actual); \
    } \
} while (0)

#define ASSERT_STREQ(actual, expected, message) do { \
    tests_run++; \
    if (strcmp(actual, expected) == 0) { \
        tests_passed++; \
        printf("%s✓%s ", COLOR_GREEN, COLOR_RESET); \
    } else { \
        tests_failed++; \
        printf("%s✗%s ", COLOR_RED, COLOR_RESET); \
        printf("%sFAIL:%s %s (expected: \"%s\", got: \"%s\")\n", \
               COLOR_RED, COLOR_RESET, message, expected, actual); \
    } \
} while (0)

// ============================================================================
// Test: is_word_boundary()
// ============================================================================

void test_is_word_boundary() {
    printf("\n%sTesting is_word_boundary()%s\n", COLOR_CYAN, COLOR_RESET);

    // Alphanumeric and underscore should NOT be boundaries
    ASSERT(!is_word_boundary('a'), "Lowercase letter is not boundary");
    ASSERT(!is_word_boundary('Z'), "Uppercase letter is not boundary");
    ASSERT(!is_word_boundary('5'), "Digit is not boundary");
    ASSERT(!is_word_boundary('_'), "Underscore is not boundary");

    // Everything else should be boundaries
    ASSERT(is_word_boundary(' '), "Space is boundary");
    ASSERT(is_word_boundary('\t'), "Tab is boundary");
    ASSERT(is_word_boundary('\n'), "Newline is boundary");
    ASSERT(is_word_boundary('.'), "Period is boundary");
    ASSERT(is_word_boundary(','), "Comma is boundary");
    ASSERT(is_word_boundary('-'), "Hyphen is boundary");
    ASSERT(is_word_boundary('/'), "Slash is boundary");
    ASSERT(is_word_boundary('('), "Open paren is boundary");
    ASSERT(is_word_boundary(')'), "Close paren is boundary");
}

// ============================================================================
// Test: move_backward_word()
// ============================================================================

void test_move_backward_word() {
    printf("\n%sTesting move_backward_word()%s\n", COLOR_CYAN, COLOR_RESET);

    // Test 1: Simple case - cursor at end of single word
    const char *buf1 = "hello";
    ASSERT_EQ(move_backward_word(buf1, 5), 0, "From end of 'hello' to start");

    // Test 2: Two words separated by space
    const char *buf2 = "hello world";
    ASSERT_EQ(move_backward_word(buf2, 11), 6, "From end of 'world' to start of 'world'");
    ASSERT_EQ(move_backward_word(buf2, 6), 0, "From start of 'world' to start of 'hello'");

    // Test 3: Multiple spaces
    const char *buf3 = "hello   world";
    ASSERT_EQ(move_backward_word(buf3, 13), 8, "Skip multiple spaces to start of 'world'");

    // Test 4: Punctuation boundaries
    const char *buf4 = "hello.world";
    ASSERT_EQ(move_backward_word(buf4, 11), 6, "Period is word boundary");

    // Test 5: Cursor in middle of word
    const char *buf5 = "hello world";
    ASSERT_EQ(move_backward_word(buf5, 8), 6, "From middle of 'world' to start");

    // Test 6: Underscore is part of word
    const char *buf6 = "var_name test";
    ASSERT_EQ(move_backward_word(buf6, 13), 9, "Underscore included in word");
    ASSERT_EQ(move_backward_word(buf6, 9), 0, "From 'test' to 'var_name'");

    // Test 7: Start of buffer
    ASSERT_EQ(move_backward_word("hello", 0), 0, "Already at start");

    // Test 8: Mixed punctuation
    const char *buf8 = "foo-bar baz";
    ASSERT_EQ(move_backward_word(buf8, 11), 8, "From 'baz' back one word");
    ASSERT_EQ(move_backward_word(buf8, 8), 4, "Hyphen is boundary");

    // Test 9: Trailing spaces
    const char *buf9 = "hello ";
    ASSERT_EQ(move_backward_word(buf9, 6), 0, "Skip trailing space");
}

// ============================================================================
// Test: move_forward_word()
// ============================================================================

void test_move_forward_word() {
    printf("\n%sTesting move_forward_word()%s\n", COLOR_CYAN, COLOR_RESET);

    // Test 1: Simple case - cursor at start of single word
    const char *buf1 = "hello";
    ASSERT_EQ(move_forward_word(buf1, 0, 5), 5, "From start to end of 'hello'");

    // Test 2: Two words separated by space
    const char *buf2 = "hello world";
    ASSERT_EQ(move_forward_word(buf2, 0, 11), 6, "From start of 'hello' to start of 'world'");
    ASSERT_EQ(move_forward_word(buf2, 6, 11), 11, "From start of 'world' to end");

    // Test 3: Multiple spaces
    const char *buf3 = "hello   world";
    ASSERT_EQ(move_forward_word(buf3, 0, 13), 8, "Skip multiple spaces to 'world'");

    // Test 4: Punctuation boundaries
    const char *buf4 = "hello.world";
    ASSERT_EQ(move_forward_word(buf4, 0, 11), 6, "Period is word boundary");

    // Test 5: Cursor in middle of word
    const char *buf5 = "hello world";
    ASSERT_EQ(move_forward_word(buf5, 2, 11), 6, "From middle of 'hello' to start of 'world'");

    // Test 6: Underscore is part of word
    const char *buf6 = "var_name test";
    ASSERT_EQ(move_forward_word(buf6, 0, 13), 9, "Underscore included in word");
    ASSERT_EQ(move_forward_word(buf6, 9, 13), 13, "From 'test' to end");

    // Test 7: End of buffer
    ASSERT_EQ(move_forward_word("hello", 5, 5), 5, "Already at end");

    // Test 8: Mixed punctuation
    const char *buf8 = "foo-bar baz";
    ASSERT_EQ(move_forward_word(buf8, 0, 11), 4, "Hyphen is boundary");
    ASSERT_EQ(move_forward_word(buf8, 4, 11), 8, "From after hyphen to 'baz'");

    // Test 9: Leading spaces
    const char *buf9 = " hello";
    ASSERT_EQ(move_forward_word(buf9, 0, 6), 1, "Skip leading space");
}

// ============================================================================
// Test: visible_strlen()
// ============================================================================

void test_visible_strlen() {
    printf("\n%sTesting visible_strlen()%s\n", COLOR_CYAN, COLOR_RESET);

    // Test 1: Plain string (no ANSI codes)
    ASSERT_EQ(visible_strlen("hello"), 5, "Plain string");
    ASSERT_EQ(visible_strlen(""), 0, "Empty string");
    ASSERT_EQ(visible_strlen("a"), 1, "Single character");

    // Test 2: String with simple ANSI color codes
    ASSERT_EQ(visible_strlen("\033[32mhello\033[0m"), 5, "Green colored 'hello'");
    ASSERT_EQ(visible_strlen("\033[31mred\033[0m"), 3, "Red colored 'red'");

    // Test 3: Multiple ANSI codes
    ASSERT_EQ(visible_strlen("\033[1m\033[32mbold green\033[0m"), 10, "Multiple codes");

    // Test 4: Real prompt example
    const char *prompt = "\033[32m> \033[0m";
    ASSERT_EQ(visible_strlen(prompt), 2, "Typical colored prompt");

    // Test 5: ANSI code at different positions
    ASSERT_EQ(visible_strlen("hello\033[0m world"), 11, "Code in middle");
    ASSERT_EQ(visible_strlen("\033[32mstart"), 5, "Code at start");
    ASSERT_EQ(visible_strlen("end\033[0m"), 3, "Code at end");

    // Test 6: Complex ANSI codes
    ASSERT_EQ(visible_strlen("\033[38;5;123mcolor\033[0m"), 5, "256-color code");
    ASSERT_EQ(visible_strlen("\033[1;31;40mtext\033[0m"), 4, "Multiple params");

    // Test 7: Back-to-back ANSI codes
    ASSERT_EQ(visible_strlen("\033[0m\033[32m\033[1mhello"), 5, "Consecutive codes");

    // Test 8: Mixed content
    ASSERT_EQ(visible_strlen("a\033[32mb\033[0mc"), 3, "Alternating visible and codes");
}

// ============================================================================
// Test: Word Movement Integration
// ============================================================================

void test_word_movement_integration() {
    printf("\n%sTesting word movement integration%s\n", COLOR_CYAN, COLOR_RESET);

    // Test scenario: "git commit -m 'initial commit'"
    const char *cmd = "git commit -m 'initial commit'";
    int len = strlen(cmd);

    // Forward from start
    // Note: '-' and '\'' are word boundaries, so they get skipped
    int pos = 0;
    pos = move_forward_word(cmd, pos, len);  // -> 4 (to "commit")
    ASSERT_EQ(pos, 4, "First forward: to 'commit'");

    pos = move_forward_word(cmd, pos, len);  // -> 12 (to "m", skips '-')
    ASSERT_EQ(pos, 12, "Second forward: to 'm' (after hyphen)");

    pos = move_forward_word(cmd, pos, len);  // -> 15 (to "initial", skips '\'')
    ASSERT_EQ(pos, 15, "Third forward: to 'initial' (after quote)");

    // Now go backward
    pos = move_backward_word(cmd, pos);  // -> 12 (back to "m")
    ASSERT_EQ(pos, 12, "First backward: to 'm'");

    pos = move_backward_word(cmd, pos);  // -> 4 (back to "commit")
    ASSERT_EQ(pos, 4, "Second backward: to 'commit'");

    pos = move_backward_word(cmd, pos);  // -> 0 (back to "git")
    ASSERT_EQ(pos, 0, "Third backward: to 'git'");
}

// ============================================================================
// Test: Multiline Word Movement
// ============================================================================

void test_multiline_word_movement() {
    printf("\n%sTesting multiline word movement%s\n", COLOR_CYAN, COLOR_RESET);

    // Test with newlines
    const char *multiline = "hello\nworld\ntest";
    int len = strlen(multiline);

    // Forward movement across newlines
    int pos = 0;
    pos = move_forward_word(multiline, pos, len);  // -> 6 (after newline, at 'w')
    ASSERT_EQ(pos, 6, "Forward: skip newline to 'world'");

    pos = move_forward_word(multiline, pos, len);  // -> 12 (after second newline)
    ASSERT_EQ(pos, 12, "Forward: skip second newline to 'test'");

    // Backward movement across newlines
    pos = move_backward_word(multiline, pos);  // -> 6 (back to 'world')
    ASSERT_EQ(pos, 6, "Backward: back to 'world'");

    pos = move_backward_word(multiline, pos);  // -> 0 (back to 'hello')
    ASSERT_EQ(pos, 0, "Backward: back to 'hello'");
}

// ============================================================================
// Test: Edge Cases
// ============================================================================

void test_edge_cases() {
    printf("\n%sTesting edge cases%s\n", COLOR_CYAN, COLOR_RESET);

    // Empty string
    ASSERT_EQ(move_forward_word("", 0, 0), 0, "Forward on empty string");
    ASSERT_EQ(move_backward_word("", 0), 0, "Backward on empty string");

    // Single character
    ASSERT_EQ(move_forward_word("a", 0, 1), 1, "Forward on single char");
    ASSERT_EQ(move_backward_word("a", 1), 0, "Backward on single char");

    // All spaces
    const char *spaces = "     ";
    ASSERT_EQ(move_forward_word(spaces, 0, 5), 5, "Forward through all spaces");
    ASSERT_EQ(move_backward_word(spaces, 5), 0, "Backward through all spaces");

    // All punctuation
    const char *punct = "...!!!";
    ASSERT_EQ(move_forward_word(punct, 0, 6), 6, "Forward through punctuation");
    ASSERT_EQ(move_backward_word(punct, 6), 0, "Backward through punctuation");

    // Single word, no spaces
    const char *single = "supercalifragilisticexpialidocious";
    int single_len = strlen(single);
    ASSERT_EQ(move_forward_word(single, 0, single_len), single_len,
              "Forward on very long word");
    ASSERT_EQ(move_backward_word(single, single_len), 0,
              "Backward on very long word");
}

// ============================================================================
// Test: Complex Real-World Scenarios
// ============================================================================

void test_complex_scenarios() {
    printf("\n%sTesting complex real-world scenarios%s\n", COLOR_CYAN, COLOR_RESET);

    // Test 1: File paths with various separators
    const char *path = "/usr/local/bin/my-script.sh";
    int path_len = strlen(path);

    int pos = 0;
    pos = move_forward_word(path, pos, path_len);
    ASSERT_EQ(pos, 1, "Forward through '/' to 'usr'");

    pos = move_forward_word(path, pos, path_len);
    ASSERT_EQ(pos, 5, "Forward to 'local'");

    // Test 2: C function declaration
    const char *func = "int my_function(char *ptr);";
    int func_len = strlen(func);

    pos = 0;
    pos = move_forward_word(func, pos, func_len);
    ASSERT_EQ(pos, 4, "Forward to 'my_function'");

    pos = move_forward_word(func, pos, func_len);
    ASSERT_EQ(pos, 16, "Forward to 'char', skip parenthesis");

    // Test 3: Mixed tabs and spaces
    const char *mixed = "word1\t\tword2   word3";
    int mixed_len = strlen(mixed);

    pos = 0;
    pos = move_forward_word(mixed, pos, mixed_len);
    ASSERT_EQ(pos, 7, "Forward through tabs");

    pos = move_forward_word(mixed, pos, mixed_len);
    ASSERT_EQ(pos, 15, "Forward through multiple spaces");

    // Test 4: Email-like string
    const char *email = "user@example.com";
    int email_len = strlen(email);

    pos = 0;
    pos = move_forward_word(email, pos, email_len);
    ASSERT_EQ(pos, 5, "Forward to '@' boundary");

    pos = move_forward_word(email, pos, email_len);
    ASSERT_EQ(pos, 13, "Forward to '.' boundary");

    // Test 5: Consecutive boundaries
    const char *punct = "word...!!!word";
    int punct_len = strlen(punct);

    pos = 0;
    pos = move_forward_word(punct, pos, punct_len);
    ASSERT_EQ(pos, 10, "Forward through multiple punctuation marks");

    // Test 6: Backward through complex string
    const char *complex = "foo_bar.baz-qux";
    pos = strlen(complex);

    pos = move_backward_word(complex, pos);
    ASSERT_EQ(pos, 12, "Backward to 'qux'");

    pos = move_backward_word(complex, pos);
    ASSERT_EQ(pos, 8, "Backward to 'baz'");

    pos = move_backward_word(complex, pos);
    ASSERT_EQ(pos, 0, "Backward to 'foo_bar' (underscore included)");
}

// ============================================================================
// Test: ANSI Escape Sequence Edge Cases
// ============================================================================

void test_ansi_edge_cases() {
    printf("\n%sTesting ANSI escape sequence edge cases%s\n", COLOR_CYAN, COLOR_RESET);

    // Test 1: Nested/malformed ANSI sequences
    ASSERT_EQ(visible_strlen("\033[1\033[32mtext\033[0m"), 4, "Nested escape start");

    // Test 2: ANSI sequence at very start
    ASSERT_EQ(visible_strlen("\033[0mstart"), 5, "Escape at start");

    // Test 3: ANSI sequence at very end
    ASSERT_EQ(visible_strlen("end\033[0m"), 3, "Escape at end");

    // Test 4: Multiple consecutive escapes
    ASSERT_EQ(visible_strlen("\033[0m\033[31m\033[1m"), 0, "Multiple consecutive escapes");

    // Test 5: Text between escape sequences
    ASSERT_EQ(visible_strlen("\033[31mr\033[32mg\033[34mb"), 3, "Single chars with colors");

    // Test 6: Real-world colored prompt
    const char *prompt = "\033[1;32muser@host\033[0m:\033[1;34m~/dir\033[0m$ ";
    ASSERT_EQ(visible_strlen(prompt), 17, "Complex colored prompt");

    // Test 7: Cursor movement sequences (more complex)
    ASSERT_EQ(visible_strlen("\033[2J\033[H"), 0, "Clear screen sequences");

    // Test 8: Bold, italic, underline combinations
    ASSERT_EQ(visible_strlen("\033[1;3;4mstyle\033[0m"), 5, "Multiple style codes");
}

// ============================================================================
// Test: Boundary Character Combinations
// ============================================================================

void test_boundary_combinations() {
    printf("\n%sTesting boundary character combinations%s\n", COLOR_CYAN, COLOR_RESET);

    // Test 1: Underscore vs hyphen (underscore is NOT a boundary)
    const char *mixed = "foo_bar-baz";
    int len = strlen(mixed);

    int pos = 0;
    pos = move_forward_word(mixed, pos, len);
    ASSERT_EQ(pos, 8, "Underscore included in word, hyphen stops");

    pos = move_forward_word(mixed, pos, len);
    ASSERT_EQ(pos, 11, "Move to next word after hyphen");

    // Test 2: Numbers in words
    const char *alphanum = "test123 var456";
    int alphanum_len = strlen(alphanum);

    pos = 0;
    pos = move_forward_word(alphanum, pos, alphanum_len);
    ASSERT_EQ(pos, 8, "Numbers included in word");

    // Test 3: Camel case (treated as single word)
    const char *camel = "myVariableName";
    pos = 0;
    pos = move_forward_word(camel, pos, strlen(camel));
    ASSERT_EQ(pos, 14, "CamelCase treated as single word");

    // Test 4: Multiple consecutive underscores
    const char *underscores = "word___word";
    pos = 0;
    pos = move_forward_word(underscores, pos, strlen(underscores));
    ASSERT_EQ(pos, 11, "Multiple underscores included in word");

    // Test 5: Start on boundary
    const char *start_bound = "   word";
    pos = 0;
    pos = move_forward_word(start_bound, pos, strlen(start_bound));
    ASSERT_EQ(pos, 3, "Start on boundary moves to next word");
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("%s========================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("%s Input Handler Unit Tests%s\n", COLOR_CYAN, COLOR_RESET);
    printf("%s========================================%s\n", COLOR_CYAN, COLOR_RESET);

    // Run all test suites
    test_is_word_boundary();
    test_move_backward_word();
    test_move_forward_word();
    test_visible_strlen();
    test_word_movement_integration();
    test_multiline_word_movement();
    test_edge_cases();
    test_complex_scenarios();
    test_ansi_edge_cases();
    test_boundary_combinations();

    // Print summary
    printf("\n%s========================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("%sTest Summary%s\n", COLOR_CYAN, COLOR_RESET);
    printf("%s========================================%s\n", COLOR_CYAN, COLOR_RESET);
    printf("Total tests:  %s%d%s\n", COLOR_YELLOW, tests_run, COLOR_RESET);
    printf("Passed:       %s%d%s\n", COLOR_GREEN, tests_passed, COLOR_RESET);
    printf("Failed:       %s%d%s\n", tests_failed > 0 ? COLOR_RED : COLOR_GREEN,
           tests_failed, COLOR_RESET);

    if (tests_failed == 0) {
        printf("\n%s✓ All tests passed!%s\n", COLOR_GREEN, COLOR_RESET);
        return 0;
    } else {
        printf("\n%s✗ Some tests failed%s\n", COLOR_RED, COLOR_RESET);
        return 1;
    }
}
