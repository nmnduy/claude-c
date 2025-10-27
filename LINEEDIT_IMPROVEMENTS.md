# Line Editor Improvements Plan

Based on analysis of ncurses implementation patterns and current gaps in our line editor.

## Current State

**Strengths:**
- ✅ Good terminal state management with signal handlers and cleanup
- ✅ Bracketed paste mode support for multiline input
- ✅ Word-based operations (Alt+b/f/d)
- ✅ Comprehensive wrapping calculation (well-tested)
- ✅ Tab completion framework
- ✅ Basic cursor movement (arrows, Ctrl+a/e, Home/End)

**Identified Gaps:**
- ❌ No UTF-8/Unicode support (ASCII only, chars 32-127)
- ❌ Missing Forward Delete key
- ❌ No command history (Up/Down arrows)
- ❌ Monolithic function (230 lines mixing input/buffer/display)
- ❌ Basic escape sequence handling without timeout
- ❌ No input queue/ungetch mechanism

## High-Priority Improvements

### 1. UTF-8/Unicode Support
**Problem:** Current code only handles ASCII (lines 536-545: `c >= 32 && c < 127`)

**Solution:**
- Detect UTF-8 multibyte sequences (2-4 bytes)
- Read continuation bytes (10xxxxxx pattern)
- Update buffer insertion to handle multibyte chars
- Fix cursor positioning to count characters, not bytes
- Update display logic for proper rendering

**ncurses reference:** Uses `wchar_t` and wide character functions

### 2. Forward Delete Key
**Problem:** Only backspace works, no Delete key support

**Solution:**
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

### 5. Command History
**Problem:** No Up/Down arrow history navigation

**Solution:**
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

## Implementation Order

1. **UTF-8 support** - Foundational for international users
2. **Forward delete & escape handling** - Quick wins, improves UX
3. **Code refactoring** - Makes subsequent work easier
4. **Command history** - Major UX improvement
5. **Better bounds checking** - Robustness & safety

## Testing Strategy

- Extend `tests/test_lineedit.c` with:
  - UTF-8 character insertion tests
  - Forward delete tests
  - History navigation tests
  - Edge case handling
- Manual testing with:
  - Unicode characters (emoji, Chinese, Arabic)
  - Different terminal emulators
  - Terminal resize during input
  - Very long input lines

## References

- **ncurses source:** `/Users/dunguyen/code/ncurses/`
  - `ncurses/base/lib_getch.c` - Input handling patterns
  - `form/frm_driver.c` - Buffer management patterns
  - `form/frm_cursor.c` - Cursor positioning
- **UTF-8 Specification:** RFC 3629
- **ANSI Escape Codes:** ECMA-48 standard
