# CLAUDE.md

Project instructions for Claude Code when working with this codebase.

## Guidelines for This Document

**Purpose**: High-level executive overview and table of contents for the codebase.

**Keep it minimal:**
- Directional, not prescriptive - point to where things are, don't duplicate documentation
- Table of contents > detailed specs - link to source files and docs, don't copy them
- Executive summary > implementation details - what and where, not how
- Updates should be rare and only for structural/architectural changes

**Full documentation lives in**: Source code comments, `docs/*.md`, and individual README files.

## Quick Navigation

**Current tasks**: `./todo.md`
**Main implementation**: `src/claude.c` (core agent loop, API calls)
**Tools**: Built-in tools in `src/claude.c`, MCP support in `src/mcp.h`, `src/mcp.c`
**MCP integration**: `docs/mcp.md` (external tool servers)
**TODO system**: `src/todo.h`, `src/todo.c`
**TUI & Normal Mode**: `src/tui.h`, `src/tui.c`, `docs/normal-mode.md`
**Color themes**: `src/colorscheme.h`, `src/builtin_themes.h`, `src/builtin_themes.c`
**Token usage tracking**: `docs/token-usage.md`, `src/persistence.c`
**Tests**: `tests/test_*.c`
**Build**: `Makefile`

## Project Overview

Pure C implementation of a coding agent using Anthropic's Claude API.

**Stack:**
- C11 + POSIX
- libcurl (HTTP), cJSON (parsing), pthread, ncurses (TUI)
- 7 core tools implemented
- Prompt caching enabled by default
- Bash command timeout protection (configurable via `CLAUDE_C_BASH_TIMEOUT`)

## C Coding Standards

**Key principles:**
- Initialize all pointers to NULL
- Use goto cleanup pattern for resource management
- Bounded string functions only (snprintf, strncpy)
- Zero-initialize structs with `= {0}`
- Check all malloc/calloc returns

**Required compilation:**
- Flags: `-Wall -Wextra -Werror`
- Sanitizers: `-fsanitize=address,undefined` for testing
- Static analysis: `clang --analyze` or `gcc -fanalyzer`

**Testing requirements:**
- Zero warnings, zero leaks (Valgrind)
- All tests pass with sanitizers enabled
- See `Makefile` for test targets
- Run `make fmt-whitespace` once after making code changes.

## Building and Testing

**Quick start:**
```bash
make check-deps   # Verify dependencies (libcurl, cJSON, pthread)
make              # Build: output to build/claude-c
make test         # Run unit tests (tests/ directory)
```

**Running:**
```bash
export OPENAI_API_KEY="your-api-key"
./build/claude-c "your prompt"
```

**Test locations:**
- `tests/test_edit.c` - Edit tool tests (regex, replace_all)
- `tests/test_todo.c` - TODO list system tests
- Build system: Uses `-DTEST_BUILD` to expose internal functions

## Configuration

**Environment variables:**
- **API**: `OPENAI_API_KEY` (required), `OPENAI_MODEL`, `OPENAI_API_BASE`
- **OpenAI Authentication**: `OPENAI_AUTH_HEADER` - Custom auth header template (e.g., "x-api-key: %s" or "Authorization: Bearer %s")
- **Extra Headers**: `OPENAI_EXTRA_HEADERS` - Comma-separated list of additional headers (e.g., "anthropic-version: 2023-06-01, User-Agent: my-app")
- **Caching**: `DISABLE_PROMPT_CACHING=1` to disable
- **Logging**: `CLAUDE_LOG_LEVEL` (DEBUG/INFO/WARN/ERROR), `CLAUDE_C_LOG_PATH`
- **Database**: `CLAUDE_C_DB_PATH` for API call history (SQLite)
- **Database Rotation**:
  - `CLAUDE_C_DB_MAX_DAYS` - Keep records for N days (default: 30, 0=unlimited)
  - `CLAUDE_C_DB_MAX_RECORDS` - Keep last N records (default: 1000, 0=unlimited)
  - `CLAUDE_C_DB_MAX_SIZE_MB` - Max database size in MB (default: 100, 0=unlimited)
  - `CLAUDE_C_DB_AUTO_ROTATE` - Enable auto-rotation (default: 1, set to 0 to disable)
- **Tools**: 
  - `CLAUDE_C_GREP_MAX_RESULTS` - Max grep results (default: 100)
  - `CLAUDE_C_BASH_TIMEOUT` - Timeout for bash commands in seconds (default: 30, 0=no timeout)
- **Theme**: `CLAUDE_C_THEME` pointing to Kitty .conf file
- **MCP**: `CLAUDE_MCP_ENABLED=1` to enable (disabled by default), `CLAUDE_MCP_CONFIG` for config path

**Defaults:**
- Logs: `./.claude-c/logs/claude.log` (project-local)
- Database: `./.claude-c/api_calls.db` (project-local)
- Prompt caching: Enabled
- Model: `claude-sonnet-4-20250514`
- Token usage tracking: Enabled (stores in `token_usage` table)

## Color Themes

**Format**: Kitty terminal's .conf format (zero dependencies)
**Location**: Built-in themes embedded in `src/builtin_themes.c`
**Implementation**: `src/colorscheme.h`, `src/fallback_colors.h`, `src/builtin_themes.h`

**Available built-in themes:**
- `tender` - Warm and soft, easy on the eyes (default)
- `kitty-default` - Classic high contrast
- `dracula` - Dark purple, vibrant
- `gruvbox-dark` - Warm retro, low contrast
- `solarized-dark` - Blue-tinted, balanced
- `black-metal` - Pure black, grayscale tones

**Usage:**
```bash
# Use built-in theme by name
export CLAUDE_C_THEME="dracula"
./build/claude-c

# Or use external .conf file
export CLAUDE_C_THEME="/path/to/custom-theme.conf"
./build/claude-c
```

**Technical:**
- Built-in themes: Embedded strings, no file I/O needed
- Parser: Simple `fgets()` + `sscanf()` (~250 lines)
- Uses standard Kitty keys only: `foreground`, `color0-15`
- Maps to TUI elements (assistant, user, status, error, header)
- 256-color ANSI support with RGB→256 conversion
- Compatible with 300+ external Kitty themes from kitty-themes repo

## TODO List System

**Purpose**: Task tracking for multi-step operations
**Implementation**: `src/todo.h`, `src/todo.c`
**Tests**: `tests/test_todo.c`

**States**: pending (○), in_progress (⋯), completed (✓)

**Core API:**
- `todo_init()` / `todo_free()` - Lifecycle
- `todo_add()` - Add task
- `todo_update_status()` - Update by index
- `todo_update_by_content()` - Update by matching content
- `todo_render()` - Display with colors

**Integration:**
- Part of `ConversationState` structure
- Rendered via `tui_render_todo_list()`
- Run tests: `make test-todo`

## MCP (Model Context Protocol)

**Purpose**: Connect to external servers for additional resources
**Implementation**: `src/mcp.h`, `src/mcp.c`
**Documentation**: `docs/mcp.md`
**Example config**: `examples/mcp_servers.json`

**Key features:**
- Multiple server support (stdio transport)
- Resource-based access (list/read operations)
- Compatible with Claude Desktop config format
- Matches TypeScript Claude Code implementation

**Quick start:**
```bash
export CLAUDE_MCP_ENABLED=1
mkdir -p ~/.config/claude-c
cp examples/mcp_servers.json ~/.config/claude-c/
./build/claude-c
```

**Configuration**: `~/.config/claude-c/mcp_servers.json`
- Server definitions with command, args, and environment
- See `docs/mcp.md` for details and available servers

**Tools provided:**
- `ListMcpResources` - List available resources from MCP servers (with optional server filter)
- `ReadMcpResource` - Read a specific resource by server name and URI

**Usage:**
- Resources include metadata: server, uri, name, description, mimeType
- Supports text-based resources (binary blob support planned)

## Context Building

**Automatic context injection** (matches official TypeScript CLI):

**Environment block (`<env>`):**
- Working directory, git status, platform, OS version, date

**Git information** (when in repo):
- Current branch, status (clean/modified), last 5 commits

**CLAUDE.md injection:**
- Auto-reads from working directory if present
- Wrapped in `<system-reminder>` tags
- Debug: `DEBUG_PROMPT=1 ./build/claude-c`

**Implementation** (lines ~1838-1960 in `src/claude.c`):
- `get_current_date()`, `is_git_repo()`, `exec_git_command()`
- `get_git_status()`, `get_platform()`, `get_os_version()`
- `read_claude_md()`, `build_system_prompt()`
- Assembled at startup before TUI initialization

## Dependencies

- **libcurl** - HTTP client
- **cJSON** - JSON parsing
- **pthread** - Thread support
- **ncurses** - Terminal UI (scrolling, mouse support)
- **POSIX** - File ops, glob, process management

Verify: `make check-deps`

## API Integration

**Endpoint**: `https://api.anthropic.com/v1/messages`
**API version**: `2023-06-01`
**Model**: `claude-sonnet-4-20250514`
**Max tokens**: 8192
**Mode**: Non-streaming

**Request**: Standard Messages API with tool definitions
**Response**: Extracts `content` array (text + tool_use blocks)

## Documentation

Most documentation is in `docs/`

Do not write documentation or markdown files unless explicitly instructed to.
