# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Next major milestone

Have a working version of `claude` with TUI ideally using the established library `ncurses`. Should have conversational code edit. Configuration via environment variables.

## Project Overview

This is a pure C implementation of a coding agent that interacts with Anthropic's Claude API. It's a lightweight, single-file port (~1000 lines) of the core functionality from the TypeScript Claude Code CLI.

**Key characteristics:**
- Single-file architecture in `src/claude.c`
- Standard C11 with POSIX support
- Direct API integration via libcurl
- Implements 6 core tools: Bash, Read, Write, Edit, Glob, Grep
- **Prompt caching support** - matching the official Claude Code CLI performance

## C Coding Best Practices

This project follows modern C development practices to minimize memory bugs and maximize code safety. All contributors and AI assistants must adhere to these guidelines.

### Compiler Flags and Static Analysis

**Required compilation flags:**
```bash
# Warning flags (always enabled)
-Wall -Wextra -Wshadow -Wconversion -Wuninitialized -Werror

# Hardening flags (production builds)
-fstack-protector-strong  # Stack overflow detection
-D_FORTIFY_SOURCE=2       # Runtime checks for buffer overflows
```

**Static analysis tools:**
- Run `clang --analyze` or `gcc -fanalyzer` regularly
- Use Clang Static Analyzer for deeper checks
- Consider Coverity for complex refactorings

**Sanitizers (development/testing):**
```bash
# Address Sanitizer (detects buffer overflows, use-after-free, double-free)
clang -fsanitize=address -g src/claude.c -o build/claude

# Undefined Behavior Sanitizer (catches undefined behavior)
clang -fsanitize=undefined -g src/claude.c -o build/claude

# Memory Sanitizer (detects uninitialized reads)
clang -fsanitize=memory -g src/claude.c -o build/claude

# Leak Sanitizer (finds memory leaks)
clang -fsanitize=leak -g src/claude.c -o build/claude

# Combined (recommended for testing)
clang -fsanitize=address,undefined -g src/claude.c -o build/claude
```

### Memory Management Patterns

**1. Always initialize pointers:**
```c
char *buffer = NULL;  // Good: explicit initialization
char *data;           // Bad: uninitialized pointer
```

**2. Use RAII-style cleanup with goto:**
```c
char *read_and_process(const char *path) {
    FILE *f = NULL;
    char *buffer = NULL;
    char *result = NULL;

    f = fopen(path, "r");
    if (!f) goto cleanup;

    buffer = malloc(1024);
    if (!buffer) goto cleanup;

    // ... processing ...

    result = strdup(buffer);  // Only successful result

cleanup:
    if (f) fclose(f);
    free(buffer);  // safe even if NULL
    return result;
}
```

**3. Enforce clear ownership:**
- Document who owns each allocated pointer
- Use naming conventions: `*_create()` pairs with `*_destroy()`
- Avoid shared ownership; prefer explicit reference counting if needed

**4. Prefer stack allocation for small buffers:**
```c
char buffer[256];     // Good for small, fixed-size data
char *buf = malloc(); // Only when size is dynamic or large
```

**5. Zero-initialize structs:**
```c
ConversationState state = {0};  // All fields set to 0/NULL
```

### Safe String and Buffer APIs

**Never use these:**
- `gets()` - no bounds checking
- `sprintf()` - use `snprintf()` instead
- `strcpy()` - use `strncpy()` or `strlcpy()` instead

**Always use bounded versions:**
```c
// Good
snprintf(buffer, sizeof(buffer), "Value: %d", value);
strncpy(dest, src, sizeof(dest) - 1);
dest[sizeof(dest) - 1] = '\0';  // Ensure null termination

// Bad
sprintf(buffer, "Value: %d", value);
strcpy(dest, src);
```

**Check all allocations:**
```c
char *buf = malloc(size);
if (!buf) {
    fprintf(stderr, "malloc failed\n");
    return NULL;
}
```

### Runtime Testing Tools

**Valgrind (memory error detection):**
```bash
valgrind --leak-check=full --show-leak-kinds=all ./build/claude "test"
```

**Fuzzing (continuous testing):**
```bash
# Use libFuzzer or AFL++ for input validation functions
clang -fsanitize=fuzzer,address -g fuzz_target.c -o fuzz
./fuzz corpus/
```

### Code Review Checklist

When reviewing code (human or AI-generated), verify:
- [ ] All pointers initialized to NULL before use
- [ ] All `malloc()`/`calloc()` calls checked for NULL
- [ ] All allocated memory has a corresponding `free()`
- [ ] No `free()` called twice on same pointer
- [ ] No pointer used after `free()` (use-after-free)
- [ ] Buffer operations use bounded functions (`snprintf`, `strncpy`)
- [ ] Array accesses are bounds-checked
- [ ] String buffers are null-terminated
- [ ] Clear ownership documented for all heap allocations
- [ ] Cleanup paths free all resources (use goto cleanup pattern)

### Testing Requirements

**All new code must:**
1. Compile without warnings with `-Wall -Wextra -Werror`
2. Pass under AddressSanitizer (`-fsanitize=address,undefined`)
3. Have unit tests that verify correct memory management
4. Be checked with Valgrind for leaks (zero leaks tolerated)

**Continuous Integration:**
- Run sanitizer builds in CI/CD pipeline
- Integrate fuzzing for input-handling code
- Maintain 100% of tests passing

### Modern C Standards

- Use C11 or later (project uses C11)
- Prefer C23 features when widely supported
- Use `_Static_assert()` for compile-time checks
- Leverage `_Generic()` for type-safe interfaces where appropriate

## Building and Testing

### Build Commands
```bash
make              # Build the claude executable (output: build/claude)
make test         # Build and run unit tests
make clean        # Remove built files
make install      # Install to /usr/local/bin (requires sudo)
make check-deps   # Verify all dependencies are installed
```

### Development Workflow
- The build output goes to `build/` directory (not tracked in git)
- Test executable is `build/test_edit`
- Main executable is `build/claude`

### Running Tests
The test suite focuses on the Edit tool (the most complex component):
```bash
make test
```
Tests are in `tests/test_edit.c` and verify:
- Simple string replacement
- Multi-replace (`replace_all` flag)
- POSIX Extended Regex replacement
- Error handling

### Running the Agent
```bash
export ANTHROPIC_API_KEY="your-api-key"
./build/claude "your prompt here"
```

### Color Theme System

The TUI uses **Kitty terminal's theme format** - a simple key-value configuration system with zero dependencies.

**Configuration:**
```bash
export CLAUDE_THEME="/path/to/theme.conf"
./build/claude "your prompt"
```

**Default locations (checked in order):**
1. `$CLAUDE_THEME` environment variable
2. `$XDG_CONFIG_HOME/claude/theme.conf` (typically `~/.config/claude/theme.conf`)
3. `$HOME/.claude/theme.conf`
4. Built-in defaults

**Implementation location:** `src/colorscheme.h` (will be refactored to `src/theme.h`)

**Theme File Format:**
Kitty's dead-simple key-value format:
```conf
# Comments start with #
background #282a36
foreground #f8f8f2
cursor #bbbbbb

# 16 ANSI colors (color0-15)
color0 #000000
color1 #ff5555
# ...

# Optional TUI-specific overrides
assistant_fg #8be9fd
user_fg #50fa7b
status_bg #44475a
error_fg #ff5555
```

**How it works:**
1. Opens theme file and reads line by line
2. Parses each line with simple `sscanf(line, "%s %s", key, value)`
3. Skips comments (`#`) and empty lines
4. Maps color keys to TUI elements:
   - `foreground` or `assistant_fg` → Assistant text
   - `color2` or `user_fg` → User text (green)
   - `color3` or `status_bg` → Status bar (yellow)
   - `color1` or `error_fg` → Error messages (red)
   - `color4` or `header_fg` → Headers (blue/cyan)
5. Converts hex colors to RGB, then to ncurses color numbers
6. Initializes ncurses color pairs

**Implementation (~100 lines of C):**
```c
// Pseudo-code structure
typedef struct {
    int r, g, b;  // 0-255
} RGB;

RGB parse_hex_color(const char *hex);  // "#RRGGBB" -> RGB
int rgb_to_ncurses_color(RGB rgb);     // RGB -> closest color0-255

Theme load_theme(const char *path) {
    FILE *f = fopen(path, "r");
    char line[256], key[64], value[32];

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;  // Skip comments
        if (sscanf(line, "%s %s", key, value) != 2) continue;

        RGB rgb = parse_hex_color(value);

        if (strcmp(key, "foreground") == 0)
            theme.assistant = rgb_to_ncurses_color(rgb);
        // ... more mappings
    }
    return theme;
}
```

**Why Kitty's format:**
- ✅ Zero dependencies - no parser library (TOML/YAML) needed
- ✅ Trivial C implementation - just `fgets()` + `sscanf()`
- ✅ 300+ themes available from [kitty-themes](https://github.com/dexpota/kitty-themes)
- ✅ Direct compatibility with Kitty terminal themes
- ✅ Fast parsing - no complex grammar
- ✅ Human-readable and hand-editable
- ✅ Aligns with project philosophy: minimal dependencies, simple C

**Compatibility:**
Most Kitty themes work without modification. The TUI maps standard Kitty color keys to UI elements automatically.

### TODO List System

The implementation includes a task tracking system similar to the official Claude Code CLI, allowing the agent to display progress on multi-step tasks.

**Features:**
- Three task states: `pending`, `in_progress`, `completed`
- Visual indicators: ✓ (completed), ⋯ (in progress), ○ (pending)
- Colored terminal output (green, yellow, dim)
- Simple C API for task management

**Implementation location:** `src/todo.h` and `src/todo.c`

**Data structures:**
```c
typedef struct {
    char *content;       // "Run tests"
    char *active_form;   // "Running tests"
    TodoStatus status;   // pending, in_progress, completed
} TodoItem;

typedef struct {
    TodoItem *items;
    size_t count;
    size_t capacity;
} TodoList;
```

**Core API:**
- `todo_init()` / `todo_free()`: Initialize and cleanup
- `todo_add()`: Add new task
- `todo_update_status()`: Update task by index
- `todo_update_by_content()`: Update task by matching content
- `todo_render()`: Display task list with colors
- `todo_count_by_status()`: Count tasks by state

**Integration:**
- Added to `ConversationState` structure as `todo_list` field
- TUI function `tui_render_todo_list()` displays the task panel
- Tests available via `make test-todo`

**Example output:**
```
━━━ Tasks ━━━
✓ Initialize project structure
⋯ Implementing core functionality
○ Write unit tests
○ Update documentation
━━━━━━━━━━━━
```

**Testing:**
```bash
make test-todo        # Run TODO list unit tests
./build/test_todo     # Direct execution
```

The test suite (`tests/test_todo.c`) verifies:
- List initialization and cleanup
- Adding/removing items
- Status updates (by index and by content)
- Counting by status
- Visual rendering

## Context Building

The C implementation now includes automatic environment context building, matching the behavior of the official TypeScript Claude Code CLI. Before each conversation, the system automatically gathers and sends:

### Environment Information (`<env>` block)
- **Working directory**: Current working directory path
- **Git repository status**: Whether the current directory is a git repo
- **Platform**: OS platform (darwin, linux, win32, etc.)
- **OS Version**: Operating system version (via `uname -sr`)
- **Today's date**: Current date in YYYY-MM-DD format

### Git Status Information (when in a git repository)
- **Current branch**: Active git branch name
- **Repository status**: Clean or modified (based on `git status --porcelain`)
- **Recent commits**: Last 5 commits with hash and message

### CLAUDE.md Injection
**NEW**: The system now automatically reads and injects the contents of `CLAUDE.md` from the working directory into the system prompt, matching the behavior of the official Claude Code CLI. This allows you to provide project-specific instructions that the AI will follow.

- **Automatic detection**: If `CLAUDE.md` exists in the working directory, it's automatically included
- **Formatted as system-reminder**: The content is wrapped in `<system-reminder>` tags to ensure proper context handling
- **Same format as official CLI**: Uses the exact format as Claude Code, including the "claudeMd" header

**Testing the CLAUDE.md injection:**
```bash
# Set DEBUG_PROMPT to see the full system prompt including CLAUDE.md
DEBUG_PROMPT=1 ./build/claude
```

### Implementation Details
The context is built using these functions (lines 1838-1960):
- `get_current_date()`: Gets date in YYYY-MM-DD format using `strftime()`
- `is_git_repo()`: Checks for `.git` directory
- `exec_git_command()`: Executes git commands via `popen()`
- `get_git_status()`: Gathers branch, status, and recent commits
- `get_platform()`: Returns platform string via compiler macros
- `get_os_version()`: Executes `uname -sr` to get OS info
- `read_claude_md()`: Reads CLAUDE.md file from working directory if present
- `build_system_prompt()`: Assembles complete system message with all context including CLAUDE.md

The context is automatically added as a system message at conversation start (in `main()` before TUI initialization), ensuring the AI has full awareness of the working environment.

## Code Architecture

### Single-File Structure (src/claude.c)

The code is organized into clear sections with comments:

1. **Lines 1-90**: Headers, configuration constants, data structures
   - `ConversationState`: Main state container
   - `Message`: Conversation history with role and content
   - `ContentBlock`: Supports text, tool_use, and tool_result types

2. **Lines 90-165**: Utility functions
   - File I/O helpers (`read_file`, `write_file`)
   - Path resolution (`resolve_path`)
   - CURL callback for API responses

3. **Lines 166-511**: Tool implementations
   - `tool_bash` (166): Executes shell commands via `popen`
   - `tool_read` (216): Reads file contents
   - `tool_write` (249): Writes/overwrites files
   - `tool_edit` (413): String/regex replacement with multi-replace support
   - `tool_glob` (512): File pattern matching via POSIX `glob()`
   - `tool_grep` (543): Executes system grep/ripgrep

4. **Lines 592-760**: Tool registry and API schemas
   - Tool dispatch table
   - JSON schema definitions for each tool
   - `get_tool_definitions()`: Builds tool array for API

5. **Lines 760-861**: API client
   - `call_claude_api()`: Builds request, calls API, parses response
   - Uses libcurl for HTTP, cJSON for JSON
   - Model: `claude-sonnet-4-20250514`
   - Max tokens: 8192

6. **Lines 862-920**: Message management
   - `add_user_message()`: Adds user text to conversation
   - `add_assistant_message()`: Parses API response into message history
   - `add_tool_results()`: Adds tool execution results

7. **Lines 921-1000**: Main conversation loop
   - `run_conversation()`: Handles tool execution loop
   - Executes tools sequentially when Claude requests them
   - Recursively calls API until no more tool uses
   - Entry point and cleanup

### Key Design Patterns

**Message/Content Model**: Messages have a role (USER/ASSISTANT) and contain an array of content blocks. Content blocks can be:
- `CONTENT_TEXT`: Plain text
- `CONTENT_TOOL_USE`: Tool invocation from Claude
- `CONTENT_TOOL_RESULT`: Tool execution result

**Tool Execution Flow**:
1. User sends initial prompt
2. API returns response (may include tool_use blocks)
3. Execute each tool sequentially
4. Add tool results to conversation
5. Call API again with results
6. Repeat until Claude returns only text (no more tool uses)

**Memory Management**: All heap allocations use malloc/calloc and must be explicitly freed. The main cleanup happens in the conversation loop after processing.

## Edit Tool Implementation

The Edit tool is the most sophisticated, supporting both simple string replacement and POSIX Extended Regex:

**Parameters:**
- `file_path`: Path to file (relative or absolute)
- `old_string`: String or regex pattern to find
- `new_string`: Replacement string
- `replace_all`: (optional, default: false) Replace all occurrences
- `use_regex`: (optional, default: false) Treat old_string as regex pattern

**Implementation notes:**
- Uses `str_replace_all()` for simple string replacement (lines 290-326)
- Uses `regex_replace()` for regex patterns (lines 329-411)
- Returns replacement count in result JSON
- Regex uses POSIX ERE (`REG_EXTENDED`)

## Test Build System

Testing uses a clever build trick to expose internal functions:

1. Compile `claude.c` with `-DTEST_BUILD -Dmain=unused_main`
   - `TEST_BUILD` changes `static` to empty (exposes internal functions)
   - Renames `main` to avoid conflict with test's main
2. Compile test suite separately
3. Link both object files together

This allows tests to call internal functions like `tool_edit()` without modifying the production code.

## Dependencies

- **libcurl**: HTTP client for API requests
- **cJSON**: JSON parsing and generation
- **pthread**: Thread support (infrastructure in place, not actively used yet)
- **POSIX**: File operations, glob, process management

All dependencies must be installed before building. Use `make check-deps` to verify.

## API Integration Details

**Anthropic Messages API:**
- Endpoint: `https://api.anthropic.com/v1/messages`
- API version: `2023-06-01`
- Model: `claude-sonnet-4-20250514`
- Max tokens: 8192
- Non-streaming (waits for full response)

**Request format**: Standard Messages API with tool definitions included in every request.

**Response parsing**: Extracts `content` array from response, handles both text and tool_use blocks.

### Prompt Caching

The C implementation now includes **prompt caching** support, matching the official TypeScript Claude Code CLI's performance optimization strategy.

**Implementation details** (lines 1241-1400 in `src/claude.c`):

1. **Cache breakpoints** are strategically placed at:
   - **System prompt**: The environment context and instructions get cached
   - **Tool definitions**: All 6 tool schemas (Bash, Read, Write, Edit, Glob, Grep) are cached
   - **Recent messages**: The last 3 conversation turns get cache markers

2. **Cache control format**:
   ```json
   {
     "type": "ephemeral",
     "cache_control": {"type": "ephemeral"}
   }
   ```

3. **Performance benefits**:
   - **Reduced latency**: Cached content is not reprocessed on subsequent turns
   - **Lower costs**: Cache reads are ~90% cheaper than regular input tokens
     - Regular input: $3/MTok (Sonnet 3.5)
     - Cache read: $0.30/MTok (10x cheaper!)
     - Cache write: $3.75/MTok
   - **Persistent across turns**: 5-minute cache TTL for conversation continuity

4. **Environment control**:
   ```bash
   # Disable caching globally (for debugging or comparison)
   export DISABLE_PROMPT_CACHING=1
   ./build/claude "your prompt"

   # Enable caching (default)
   unset DISABLE_PROMPT_CACHING
   ./build/claude "your prompt"
   ```

5. **What gets cached**:
   - System prompt with git status, environment info (~1-2KB)
   - All 6 tool definitions with parameters (~3-4KB)
   - Last 3 messages in conversation history

   This typically results in 5-10KB of cached context that doesn't need to be reprocessed on each API call.

**Why this matters**: Without caching, the C version was re-processing the entire context (system prompt + tools + history) on every turn, causing noticeably slower API response times compared to the official CLI. With caching enabled, performance now matches the TypeScript implementation.

## Known Limitations

1. **Sequential tool execution**: Tools run one at a time, not in parallel
2. **No streaming**: Waits for complete API response (no SSE support)
3. **Basic error handling**: Errors printed to stderr, minimal recovery
4. **No Read line ranges**: Read tool always reads entire file
5. **No permission system**: All tools execute without asking
6. **No MCP support**: No Model Context Protocol servers
7. **No hooks**: No user-configurable event handlers
8. **Single conversation**: No session management or history across runs

## Future Enhancement Areas

Per README, planned enhancements include:
- Streaming API responses (SSE)
- Parallel tool execution with pthreads (infrastructure exists)
- Interactive conversation mode
- Configuration file support
- WebFetch tool with libcurl
- Memory-efficient large file handling
- Read tool with line offset/limit support
