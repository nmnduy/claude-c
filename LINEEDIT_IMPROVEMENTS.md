# Line Editor Improvements Plan

Based on analysis of ncurses implementation patterns and current gaps in our line editor.

## 🎉 Completion Summary

**Status:** 3 out of 5 high-priority improvements completed!

✅ **UTF-8/Unicode Support** - Full multibyte character handling
✅ **Forward Delete Key** - ESC[3~ sequence support
✅ **Command History** - Up/Down arrow navigation with 100-entry buffer
⏳ **Code Refactoring** - Pending (lower priority)
⏳ **Escape Sequence Timeout** - Pending (lower priority)

**Test Coverage:** 67/67 tests passing
- 11 UTF-8 tests
- 13 History tests
- 43 Wrapping/cursor tests (existing)

**Commits:**
- `902bf60` - Add major lineedit improvements: UTF-8, Delete key, and History
- `ed407fc` - Add comprehensive unit tests for lineedit improvements
- `021c477` - Update LINEEDIT_IMPROVEMENTS.md with completion status

**Implementation Details:**
- UTF-8 helpers: `src/lineedit.c:135-177` (char_length, is_continuation, read_utf8_char)
- UTF-8 insertion: `src/lineedit.c:583-619` (handles multibyte input)
- Forward Delete: `src/lineedit.c:511-527` (ESC[3~ handler)
- History struct: `src/lineedit.h:21-30, src/lineedit.c:21-72`
- History navigation: `src/lineedit.c:545-593` (Up/Down arrow handlers)

## Current State

**Strengths:**
- ✅ Good terminal state management with signal handlers and cleanup
- ✅ Bracketed paste mode support for multiline input
- ✅ Word-based operations (Alt+b/f/d)
- ✅ Comprehensive wrapping calculation (well-tested)
- ✅ Tab completion framework
- ✅ Basic cursor movement (arrows, Ctrl+a/e, Home/End)
- ✅ **NEW: Full UTF-8/Unicode support** (1-4 byte characters)
- ✅ **NEW: Forward Delete key** (ESC[3~)
- ✅ **NEW: Command history** (Up/Down arrows, 100 entries)

**Originally Identified Gaps:**
- ✅ ~~No UTF-8/Unicode support~~ → **FIXED: Full UTF-8 support implemented**
- ✅ ~~Missing Forward Delete key~~ → **FIXED: ESC[3~ now handled**
- ✅ ~~No command history~~ → **FIXED: Up/Down arrow navigation with 100-entry buffer**
- ❌ Monolithic function (230 lines mixing input/buffer/display) - *Remaining*
- ❌ Basic escape sequence handling without timeout - *Remaining*
- ❌ No input queue/ungetch mechanism - *Remaining*

## High-Priority Improvements

### 1. UTF-8/Unicode Support ✅ COMPLETED
**Problem:** Current code only handles ASCII (lines 536-545: `c >= 32 && c < 127`)

**Solution (Implemented):**
- Detect UTF-8 multibyte sequences (2-4 bytes)
- Read continuation bytes (10xxxxxx pattern)
- Update buffer insertion to handle multibyte chars
- Fix cursor positioning to count characters, not bytes
- Update display logic for proper rendering

**ncurses reference:** Uses `wchar_t` and wide character functions

### 2. Forward Delete Key ✅ COMPLETED
**Problem:** Only backspace works, no Delete key support

**Solution (Implemented):**
- Handle `ESC[3~` escape sequence
- Implement delete-forward operation (like Ctrl+D but doesn't EOF)
- Remove character at cursor without moving cursor back

**Implementation:**
```c
// In escape sequence handler
if (seq[1] == '3') {
    // Read the ~ terminator
    // Delete character at cursor position
}
```

### 3. Refactor for Separation of Concerns
**Problem:** `lineedit_readline()` is 230 lines mixing input/buffer/display

**Solution from ncurses patterns:**
- Extract `read_key()` - input capture with timeout/retry
- Extract `buffer_insert_char()` - insert at cursor
- Extract `buffer_delete_char()` - delete at cursor
- Extract `buffer_delete_range()` - for word operations
- Keep `redraw_input_line()` - already good separation

**Benefits:**
- Easier testing of individual operations
- Clearer code organization
- Reusable buffer operations

### 4. Improve Escape Sequence Handling
**Problem:** Fixed-byte reads can hang on partial sequences

**Solution:**
- Add timeout on read() calls using select() or poll()
- Implement input queue (ungetch-like mechanism)
- Handle incomplete sequences gracefully
- State machine for complex sequences

**ncurses reference:** `lib_getch.c` has sophisticated sequence handling

### 5. Command History ✅ COMPLETED
**Problem:** No Up/Down arrow history navigation

**Solution (Implemented):**
- Implement circular history buffer (50-100 entries)
- Save current input when navigating history
- Up arrow: previous command
- Down arrow: next command (or return to current)
- Optional: persist history to `~/.claude_history`

**Data structure:**
```c
typedef struct {
    char **entries;     // Array of history strings
    int capacity;       // Max entries (50-100)
    int count;          // Current number of entries
    int position;       // Current position when navigating
} History;
```

## Medium-Priority Improvements

### 6. Better Bounds Checking
- Centralize buffer capacity checks
- Add configurable max input length
- More defensive cursor boundary checks
- Validate all buffer operations

### 7. Additional Escape Sequences
- Ctrl+Left/Right - word movement (some terminals)
- Better Home/End handling across terminals
- Alt+Backspace - delete word backward (if not working)

### 8. Visual Feedback
- Visual bell option (flash) vs beep
- Status messages for certain operations
- Better error indication

## Implementation Status

1. ✅ **UTF-8 support** - COMPLETED - Foundational for international users
2. ✅ **Forward delete** - COMPLETED - Quick win, improves UX
3. ⏳ **Code refactoring** - PENDING - Lower priority, would make future work easier
4. ✅ **Command history** - COMPLETED - Major UX improvement
5. ⏳ **Escape sequence timeout** - PENDING - Lower priority, robustness improvement
6. ⏳ **Better bounds checking** - PENDING - Lower priority, safety enhancement

## Testing

**Automated Tests (tests/test_lineedit.c):**
- ✅ 11 UTF-8 character parsing tests
- ✅ 13 Command history management tests
- ✅ 43 Terminal wrapping and cursor positioning tests
- **Total: 67/67 tests passing**

Run with: `make test-lineedit`

**Manual Testing Recommendations:**
- Unicode characters (emoji, Chinese, Arabic, etc.)
- Different terminal emulators (iTerm2, Terminal.app, Alacritty, etc.)
- Terminal resize during input
- Very long input lines
- History navigation with repeated commands

## References

- **ncurses source:** `/Users/dunguyen/code/ncurses/`
  - `ncurses/base/lib_getch.c` - Input handling patterns
  - `form/frm_driver.c` - Buffer management patterns
  - `form/frm_cursor.c` - Cursor positioning
- **UTF-8 Specification:** RFC 3629
- **ANSI Escape Codes:** ECMA-48 standard
