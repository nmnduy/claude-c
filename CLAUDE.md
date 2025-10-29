# CLAUDE.md

Project instructions for Claude Code when working with this codebase.

## Quick Navigation

**Current tasks**: `./todo.md`
**Main implementation**: `src/claude.c` (core agent loop, API calls)
**Tools**: `src/tool_*.c` (Bash, Read, Write, Edit, Glob, Grep)
**TODO system**: `src/todo.h`, `src/todo.c`
**Color themes**: `src/colorscheme.h`, `colorschemes/*.conf`
**Tests**: `tests/test_*.c`
**Build**: `Makefile`

## Project Overview

Pure C implementation of a coding agent using Anthropic's Claude API.

**Stack:**
- C11 + POSIX
- libcurl (HTTP), cJSON (parsing), pthread
- 7 core tools implemented
- Prompt caching enabled by default

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
- **Caching**: `DISABLE_PROMPT_CACHING=1` to disable
- **Logging**: `CLAUDE_LOG_LEVEL` (DEBUG/INFO/WARN/ERROR), `CLAUDE_C_LOG_PATH`
- **Database**: `CLAUDE_C_DB_PATH` for API call history (SQLite)
- **Theme**: `CLAUDE_C_THEME` pointing to Kitty .conf file

**Defaults:**
- Logs: `./.claude-c/logs/claude.log` (project-local)
- Database: `./.claude-c/api_calls.db` (project-local)
- Prompt caching: Enabled
- Model: `claude-sonnet-4-20250514`

## Color Themes

**Format**: Kitty terminal's .conf format (zero dependencies)
**Location**: `colorschemes/` directory
**Implementation**: `src/colorscheme.h`, `src/fallback_colors.h`

**Available themes:**
- `kitty-default.conf` - Classic high contrast
- `dracula.conf` - Dark purple, vibrant
- `gruvbox-dark.conf` - Warm retro, low contrast
- `solarized-dark.conf` - Blue-tinted, balanced

**Usage:**
```bash
export CLAUDE_C_THEME="./colorschemes/dracula.conf"
```

**Technical:**
- Parser: Simple `fgets()` + `sscanf()` (~250 lines)
- Uses standard Kitty keys only: `foreground`, `color0-15`
- Maps to TUI elements (assistant, user, status, error, header)
- 256-color ANSI support with RGB→256 conversion
- Compatible with 300+ Kitty themes from kitty-themes repo

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

## Prompt Caching

**Implementation**: Lines ~1241-1400 in `src/claude.c`
**Status**: Enabled by default (matches TypeScript CLI)

**Cache breakpoints:**
- System prompt (environment context)
- Tool definitions (all 6 tools)
- Last 3 conversation turns

**Performance:**
- Reduced latency (no reprocessing)
- 10x cost reduction: $0.30/MTok reads vs $3/MTok regular
- 5-minute TTL across conversation
- Typically 5-10KB cached per conversation

**Control**: `DISABLE_PROMPT_CACHING=1` to disable

## Formwork

Experimental: Consistent API for tool call extraction (replacing raw tool_use blocks)
**Status**: In development
