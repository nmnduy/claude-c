# Line Editor Improvements Plan

Based on analysis of ncurses implementation patterns and current gaps in our line editor.

## 🎉 Completion Summary

**Status:** All 5 high-priority improvements completed! ✨

✅ **UTF-8/Unicode Support** - Full multibyte character handling
✅ **Forward Delete Key** - ESC[3~ sequence support
✅ **Command History** - Up/Down arrow navigation with 100-entry buffer
✅ **Code Refactoring** - Extracted buffer operations and input queue
✅ **Escape Sequence Timeout** - Implemented with select() and input queue

**Test Coverage:** 67/67 tests passing
- 11 UTF-8 tests
- 13 History tests
- 43 Wrapping/cursor tests (existing)

**Commits:**
- `902bf60` - Add major lineedit improvements: UTF-8, Delete key, and History
- `ed407fc` - Add comprehensive unit tests for lineedit improvements
- `021c477` - Update LINEEDIT_IMPROVEMENTS.md with completion status
- (current) - Complete refactoring: buffer operations, input queue, and timeout handling

**Implementation Details:**
- UTF-8 helpers: `src/lineedit.c:135-183` (char_length, is_continuation, read_utf8_char)
- History struct: `src/lineedit.h:25-30, src/lineedit.c:25-72`
- History navigation: Integrated into main readline loop
- Input queue: `src/lineedit.h:50-51, src/lineedit.c:403-423` (ungetch mechanism)
- Timeout handling: `src/lineedit.c:429-473` (read_key with select())
- Buffer operations: `src/lineedit.c:479-556` (insert_char, delete_char, backspace, delete_range)
- Forward Delete: Uses buffer_delete_char() helper
- UTF-8 insertion: Uses buffer_insert_char() helper

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
- ✅ ~~Monolithic function~~ → **FIXED: Extracted buffer operations and input functions**
- ✅ ~~Basic escape sequence handling without timeout~~ → **FIXED: Implemented select()-based timeout**
- ✅ ~~No input queue/ungetch mechanism~~ → **FIXED: Added circular input queue**

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

### 3. Refactor for Separation of Concerns ✅ COMPLETED
**Problem:** `lineedit_readline()` was 336 lines mixing input/buffer/display

**Solution Implemented:**
- ✅ Extracted `read_key()` - input capture with timeout using select()
- ✅ Extracted `buffer_insert_char()` - insert at cursor
- ✅ Extracted `buffer_delete_char()` - forward delete at cursor
- ✅ Extracted `buffer_backspace()` - delete before cursor
- ✅ Extracted `buffer_delete_range()` - for word operations
- ✅ Already had `redraw_input_line()` - good separation maintained

**Benefits Achieved:**
- Individual operations can be tested independently
- Much clearer code organization (separate sections for each concern)
- Reusable buffer operations for future enhancements
- Main readline loop is now more readable

### 4. Improve Escape Sequence Handling ✅ COMPLETED
**Problem:** Fixed-byte reads could hang on partial sequences

**Solution Implemented:**
- ✅ Added timeout on read() calls using select() (configurable ms)
- ✅ Implemented input queue (circular buffer, 16 bytes)
- ✅ Can handle incomplete sequences gracefully (timeouts return 0)
- ✅ Infrastructure ready for state machine if needed in future

**Implementation:**
- `read_key_with_timeout()`: Uses select() for timeout support
- `read_key()`: Checks queue first, then reads from stdin
- `queue_push()` and `queue_pop()`: Circular buffer for ungetch
- Timeout defaults can be configured per read operation

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

**All high-priority improvements completed!** 🎉

1. ✅ **UTF-8 support** - COMPLETED - Foundational for international users
2. ✅ **Forward delete** - COMPLETED - Quick win, improves UX
3. ✅ **Code refactoring** - COMPLETED - Makes future work much easier
4. ✅ **Command history** - COMPLETED - Major UX improvement
5. ✅ **Escape sequence timeout** - COMPLETED - Robustness improvement with select()
6. ⏳ **Better bounds checking** - PENDING - Medium priority, safety enhancement (current checks are adequate)

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
