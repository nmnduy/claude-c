# Claude Code - Pure C Edition

A lightweight, modular implementation of a coding agent that interacts with an Open API compatible API. This is a pure C port of the core functionality from the TypeScript/Node.js Claude Code CLI.

## Features

- **Modular architecture**: Clean separation into focused modules (~2000+ lines total)
- **Core tools**: Bash, Read, Write, Edit, Glob, Grep with advanced features
- **Anthropic API integration**: Full support for Claude's Messages API with tool use
- **Efficient**: Direct API calls with libcurl, minimal dependencies
- **Portable**: Standard C11 with POSIX support
- **Theme system**: Kitty-compatible terminal themes for visual customization
- **Logging**: Comprehensive logging system for debugging and audit trails
- **Persistence**: API call history and conversation persistence

## Architecture

```
┌─────────────────────────────────────────┐
│         Main Entry Point                │
│  - CLI argument parsing                 │
│  - Environment setup                    │
└─────────────┬───────────────────────────┘
              │
┌─────────────▼───────────────────────────┐
│      Conversation Loop                  │
│  - Message management                   │
│  - API request/response handling        │
└─────────────┬───────────────────────────┘
              │
    ┌─────────┼─────────┐
    │                   │
┌───▼──────────┐  ┌────▼──────────────┐
│  API Client  │  │  Tool Executor    │
│  - libcurl   │  │  - Bash           │
│  - cJSON     │  │  - Read/Write     │
│  - Logging   │  │  - Edit           │
└──────────────┘  │  - Glob/Grep      │
                  └───────────────────┘
              │
    ┌─────────┼─────────┐
    │                   │
┌───▼──────────┐  ┌────▼──────────────┐
│   TUI/Theme  │  │   Persistence     │
│  - Colors    │  │  - API history    │
│  - Config    │  │  - Session data    │
└──────────────┘  └───────────────────┘
```

### Module Structure

```
src/
├── claude.c         # Main entry point and conversation loop
├── commands.c       # Command-line argument parsing
├── completion.c     # Auto-completion system
├── lineedit.c       # Line editing and input handling
├── logger.c         # Logging infrastructure
├── migrations.c     # Data migration utilities
├── persistence.c    # Data persistence and history
└── tui.c           # Terminal UI and theme system
```

## Dependencies

- **libcurl**: HTTP client for API requests
- **cJSON**: JSON parsing and generation
- **pthread**: Thread support (standard on most systems)
- **POSIX**: File operations, glob, process management

### Installing Dependencies

**macOS (Homebrew):**
```bash
brew install curl cjson
```

**Ubuntu/Debian:**
```bash
sudo apt-get install libcurl4-openssl-dev libcjson-dev build-essential
```

**Fedora/RHEL:**
```bash
sudo yum install libcurl-devel cjson-devel gcc make
```

## Building

```bash
cd claude-c
make
```

This will produce a `claude-c` executable in the current directory.

**Optional: Install globally**
```bash
make install
```

This installs to `/usr/local/bin/claude-c` so you can run it from anywhere.

## Usage

### Prerequisites

Set your Anthropic API key:
```bash
export ANTHROPIC_API_KEY="your-api-key-here"
```

### Running

**One-shot mode (current):**
```bash
./claude-c "your prompt here"
```

**Interactive mode:**
```bash
./claude-c
```

The interactive mode supports:
- Multi-turn conversations
- ESC key interruption: Press ESC to stop the agent during API calls or tool execution
- Slash commands: `/clear`, `/exit`, `/quit`, `/help`, `/add-dir`
- Readline-style editing: Ctrl+A, Ctrl+E, Alt+B, Alt+F, etc.

### Color Theme Support

The TUI uses **Kitty terminal's theme format** - a simple, dependency-free configuration format. This gives you access to 300+ professionally-designed themes from the kitty-themes ecosystem!

**Configuration location:**
```bash
~/.config/claude-c/theme.conf
```

**Environment variable override:**
```bash
export CLAUDE_C_THEME="/path/to/your/theme.conf"
./claude-c "your prompt"
```

**Theme Format:**
Kitty's dead-simple key-value format - no parser library needed!

```conf
# Claude TUI Theme
background #282a36
foreground #f8f8f2
cursor #bbbbbb
selection_background #44475a

# 16 ANSI colors
color0 #000000
color1 #ff5555
color2 #50fa7b
color3 #f1fa8c
# ... color4-15

# TUI-specific (optional)
assistant_fg #8be9fd
user_fg #50fa7b
status_bg #44475a
error_fg #ff5555
```

**Why Kitty's format?**
- ✅ Zero dependencies - no parser library needed
- ✅ Trivial to parse in C (~50 lines)
- ✅ 300+ themes available from kitty-themes
- ✅ Human-readable and editable
- ✅ Compatible with Kitty terminal themes
- ✅ Faster than structured formats (TOML/YAML)

**Using Kitty themes directly:**
Most Kitty themes work out of the box! Download from [kitty-themes](https://github.com/dexpota/kitty-themes):

```bash
# Download a theme
curl -o ~/.config/claude-c/dracula.conf \
  https://raw.githubusercontent.com/dexpota/kitty-themes/master/themes/Dracula.conf

# Use it
export CLAUDE_C_THEME=~/.config/claude-c/dracula.conf
./claude-c "your prompt"
```

If no theme is specified, sensible defaults are used.

### Examples

**Simple query:**
```bash
./claude-c "What files are in the current directory?"
```

**Code modification:**
```bash
./claude-c "Read main.c and add error checking to the malloc calls"
```

**Multi-step task:**
```bash
./claude-c "Find all .c files, count the lines in each, and create a summary"
```

## Available Tools

The C implementation includes these core tools:

### 1. Bash
Executes shell commands and captures output.
```json
{
  "name": "Bash",
  "parameters": {
    "command": "ls -la"
  }
}
```

### 2. Read
Reads file contents from the filesystem with optional line range support.
```json
{
  "name": "Read",
  "parameters": {
    "file_path": "/path/to/file.txt",
    "start_line": 10,     // Optional: Start reading from line 10 (1-indexed, inclusive)
    "end_line": 20        // Optional: Stop reading at line 20 (1-indexed, inclusive)
  }
}
```
**Features:**
- Read entire file or specific line ranges
- Line numbers are 1-indexed
- Returns total line count and range info
- Efficient for large files when only a portion is needed

### 3. Write
Writes content to a file (creates or overwrites).
```json
{
  "name": "Write",
  "parameters": {
    "file_path": "/path/to/file.txt",
    "content": "Hello, World!"
  }
}
```

### 4. Edit
Performs string replacements in files with optional regex and multi-replace support.
```json
{
  "name": "Edit",
  "parameters": {
    "file_path": "/path/to/file.txt",
    "old_string": "foo",
    "new_string": "bar",
    "replace_all": false,    // Optional: replace all occurrences (default: false)
    "use_regex": false       // Optional: use regex pattern matching (default: false)
  }
}
```
**Features:**
- Single or multi-replace mode
- POSIX Extended Regular Expression support
- Returns replacement count
- See [EDIT_TOOL_ENHANCEMENTS.md](EDIT_TOOL_ENHANCEMENTS.md) for detailed documentation

### 5. Glob
Finds files matching a glob pattern.
```json
{
  "name": "Glob",
  "parameters": {
    "pattern": "*.c"
  }
}
```

### 6. Grep
Searches for patterns in files (uses ripgrep/grep).
```json
{
  "name": "Grep",
  "parameters": {
    "pattern": "TODO",
    "path": "."
  }
}
```

## Implementation Details

### Message System
- **MessageRole**: USER, ASSISTANT, SYSTEM
- **ContentType**: TEXT, TOOL_USE, TOOL_RESULT
- Maintains full conversation history for context

### API Integration
- Uses Anthropic Messages API (2023-06-01 version)
- Model: `claude-sonnet-4-20250514`
- Supports tool use with automatic execution
- Non-streaming responses (streaming can be added)

### Tool Execution
- **Parallel execution**: Tool calls execute concurrently using pthreads
- Results automatically added to conversation
- Recursive execution: Claude can chain multiple tool calls
- Error handling with detailed error messages
- **Note**: Exceeds the official Node.js implementation which executes tools sequentially

### Memory Management
- Manual memory management (malloc/free)
- Proper cleanup on exit
- No memory leaks in normal operation

## Comparison to TypeScript Version

### Included Features
- ✅ Core conversation loop
- ✅ Essential tools (Bash, Read, Write, Edit, Glob, Grep)
- ✅ Anthropic API integration
- ✅ **Modular architecture** with clean separation of concerns
- ✅ Tool execution and result handling
- ✅ Message history management
- ✅ **Advanced Edit tool** with regex and multi-replace support
- ✅ **Read tool** with line range support
- ✅ **Theme system** with Kitty-compatible configuration
- ✅ **Comprehensive logging** system
- ✅ **Persistence layer** for API history and sessions
- ✅ **Parallel tool execution** with pthreads (exceeds Node.js version)

### Not Included (from original)
- ❌ MCP (Model Context Protocol) servers
- ❌ Hooks system
- ❌ Permission management
- ❌ Advanced tools (Task, WebFetch, etc.)
- ❌ **Streaming responses** (currently non-streaming only)
- ❌ Configuration files (environment variables only)
- ❌ Session management across runs

## Limitations

1. **Non-streaming responses**: Waits for full API response
2. **No session persistence**: Conversations don't persist across runs
3. **Environment-only configuration**: No config file support
4. **Basic error handling**: Errors printed to stderr, minimal recovery

## Future Enhancements

### High Priority
- [ ] **Streaming API responses** (SSE) - Better UX
- [ ] **Session persistence** - Save/load conversations

### Medium Priority  
- [ ] Configuration file support
- [ ] WebFetch tool with libcurl
- [ ] MCP (Model Context Protocol) server support
- [ ] Permission management system

### Low Priority
- [ ] Memory-efficient large file handling
- [ ] Comprehensive error recovery
- [ ] Hooks system
- [ ] Advanced tools (Task, etc.)

### Recently Completed ✅
- [x] **Modular architecture** - Clean separation into modules
- [x] **Advanced Edit tool** - Multi-replace and regex support
- [x] **Read tool enhancements** - Line range support
- [x] **Theme system** - Kitty-compatible terminal themes
- [x] **Logging system** - Comprehensive debugging support
- [x] **Persistence layer** - API history and session data
- [x] **Parallel tool execution** - Concurrent tool execution with pthreads
- [x] **ESC key interruption** - Stop agent work with ESC key during API calls or tool execution

## Code Structure

### Core Modules

**claude.c** (~300 lines)
- Main entry point and CLI argument parsing
- Conversation loop orchestration
- Environment context building

**commands.c** (~200 lines)
- Command-line argument processing
- Help system and validation

**tools/** (implemented in claude.c, ~400 lines total)
- Bash: Shell command execution
- Read: File reading with line ranges
- Write: File content writing
- Edit: String replacement with regex support
- Glob: Pattern matching
- Grep: Text search

**api/** (implemented in claude.c, ~300 lines)
- HTTP client using libcurl
- JSON parsing with cJSON
- Message formatting and response handling

**tui.c** (~250 lines)
- Terminal UI framework
- Theme system with Kitty compatibility
- Color management and display

**logger.c** (~150 lines)
- Structured logging system
- Debug and audit trail support
- Configurable log levels

**persistence.c** (~200 lines)
- API call history storage
- Session data management
- Data migration utilities

**lineedit.c** (~180 lines)
- Command line editing
- Auto-completion system
- Input handling

**completion.c** (~120 lines)
- Auto-completion engine
- Command and file completion
- Context-aware suggestions

**migrations.c** (~100 lines)
- Data schema migrations
- Version compatibility handling

### Total: ~2000+ lines of clean, modular C code

## License

This is a demonstration/educational implementation. Refer to Anthropic's terms of service for API usage.

## Contributing

This is a modular implementation designed for maintainability and extensibility. The codebase is organized into focused modules that make it easy to contribute specific features.

### Development Workflow

1. **Build and test**: `make && make test`
2. **Check dependencies**: `make check-deps`
3. **Install development tools**: `make install-dev`

### Areas for Contribution

- **Interactive TUI mode**: Multi-turn conversation interface
- **Streaming responses**: SSE implementation for real-time responses
- **Parallel execution**: Enable pthread-based concurrent tool execution
- **New tools**: WebFetch, Task, or other specialized tools
- **Performance**: Memory optimization and large file handling

For production use, consider the official TypeScript version with full features.

## Troubleshooting

**Build errors:**
- Check that all dependencies are installed: `make check-deps`
- Ensure you have a C11-compatible compiler

**Runtime errors:**
- Verify `ANTHROPIC_API_KEY` is set: `echo $ANTHROPIC_API_KEY`
- Check API key validity at https://console.anthropic.com
- Ensure you have internet connectivity for API calls

**Tool failures:**
- File tools require proper permissions
- Bash tool executes in current working directory
- Paths can be absolute or relative to cwd

## Performance

Typical performance on modern hardware:
- Startup: < 50ms (module loading overhead)
- API call: 1-5 seconds (depends on prompt complexity)
- Tool execution: Varies by tool
- Memory usage: ~10-20MB for typical conversation (modular architecture)

### Performance Notes

- **Modular overhead**: Slightly higher memory usage vs single-file
- **Theme loading**: One-time cost at startup (~5ms)
- **Logging**: Minimal impact when disabled
- **Persistence**: Background saving, non-blocking

## Current Project Status

### ✅ What's Working Now

**Core Functionality**
- Interactive TUI mode with multi-turn conversations
- ESC key interruption during API calls and tool execution
- One-shot prompt processing with Claude API
- All 6 essential tools (Bash, Read, Write, Edit, Glob, Grep)
- Advanced features: regex in Edit, line ranges in Read
- Modular architecture with clean separation
- Comprehensive logging and persistence

**User Experience**
- Interactive TUI with readline-style editing
- ESC key to stop agent work in real-time
- Slash commands for conversation control
- Kitty-compatible theme system (300+ themes available)
- Environment-based configuration
- Detailed error messages and debugging support
- Cross-platform compatibility (macOS, Linux)

### 🚧 What's Missing

**Major Features**
- Streaming API responses
- Session persistence across runs

**Infrastructure**
- Configuration file support
- MCP server integration
- Permission management
- Advanced tools (WebFetch, Task)

### 📊 Development Progress

- **Architecture**: 100% - Modular design implemented
- **Core Tools**: 100% - All essential tools working
- **API Integration**: 90% - Missing streaming
- **User Experience**: 95% - Interactive mode, themes, ESC interruption all working
- **Advanced Features**: 40% - Logging/persistence done, missing MCP/hooks

## Security Notes

- API key read from environment (not stored in code)
- No sandboxing: Bash tool has full shell access
- File tools can access any readable/writable file
- Intended for trusted environments only

## Credits

Ported from the official Claude Code TypeScript implementation.
Simplified for educational purposes and embedded systems use.
