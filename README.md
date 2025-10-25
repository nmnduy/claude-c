# Claude Code - Pure C Edition

A lightweight, single-file implementation of a coding agent that interacts with an Open API compatible API. This is a pure C port of the core functionality from the TypeScript/Node.js Claude Code CLI.

## Features

- **Single-file implementation**: Everything in one `claude.c` file (~1000 lines)
- **Core tools**: Bash, Read, Write, Edit, Glob, Grep
- **Anthropic API integration**: Full support for Claude's Messages API with tool use
- **Efficient**: Direct API calls with libcurl, minimal dependencies
- **Portable**: Standard C11 with POSIX support

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
│  - Streaming │  │  - Edit           │
└──────────────┘  │  - Glob/Grep      │
                  └───────────────────┘
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

This will produce a `claude` executable in the current directory.

**Optional: Install globally**
```bash
make install
```

This installs to `/usr/local/bin/claude` so you can run it from anywhere.

## Usage

### Prerequisites

Set your Anthropic API key:
```bash
export ANTHROPIC_API_KEY="your-api-key-here"
```

### Running

```bash
./claude "your prompt here"
```

### Color Theme Support

The TUI uses **Kitty terminal's theme format** - a simple, dependency-free configuration format. This gives you access to 300+ professionally-designed themes from the kitty-themes ecosystem!

**Configuration location:**
```bash
~/.config/claude/theme.conf
```

**Environment variable override:**
```bash
export CLAUDE_THEME="/path/to/your/theme.conf"
./claude "your prompt"
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
curl -o ~/.config/claude/dracula.conf \
  https://raw.githubusercontent.com/dexpota/kitty-themes/master/themes/Dracula.conf

# Use it
export CLAUDE_THEME=~/.config/claude/dracula.conf
./claude "your prompt"
```

If no theme is specified, sensible defaults are used.

### Examples

**Simple query:**
```bash
./claude "What files are in the current directory?"
```

**Code modification:**
```bash
./claude "Read main.c and add error checking to the malloc calls"
```

**Multi-step task:**
```bash
./claude "Find all .c files, count the lines in each, and create a summary"
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
- **Parallel execution**: Multiple tool calls execute concurrently using pthreads
- Results automatically added to conversation  
- Recursive execution: Claude can chain multiple tool calls
- Error handling with detailed error messages

### Memory Management
- Manual memory management (malloc/free)
- Proper cleanup on exit
- No memory leaks in normal operation

## Comparison to TypeScript Version

### Included Features
- ✅ Core conversation loop
- ✅ Essential tools (Bash, Read, Write, Edit, Glob, Grep)
- ✅ Anthropic API integration
- ✅ **Parallel tool execution** with pthreads
- ✅ Tool execution and result handling
- ✅ Message history management

### Not Included (from original)
- ❌ MCP (Model Context Protocol) servers
- ❌ Hooks system
- ❌ Permission management
- ❌ Advanced tools (Task, WebFetch, etc.)
- ❌ Streaming responses
- ❌ Interactive mode
- ❌ Configuration files
- ❌ Session management

## Limitations

1. **Basic error handling**: Errors printed to stderr, minimal recovery
2. **No streaming**: Waits for full API response

## Future Enhancements

- [ ] Streaming API responses (SSE)
- [x] ~~Parallel tool execution with pthreads~~ - **COMPLETED!**
- [ ] Interactive conversation mode
- [ ] Configuration file support
- [x] ~~More sophisticated Edit tool (multi-replace, regex)~~ - **COMPLETED!**
- [x] ~~Read tool with line range support~~ - **COMPLETED!**
- [ ] WebFetch tool with libcurl
- [ ] Memory-efficient large file handling
- [ ] Comprehensive error recovery

## Code Structure

```c
// Lines 1-50: Headers, configuration, data structures
// Lines 51-150: Utility functions (file I/O, path resolution)
// Lines 151-400: Tool implementations (Bash, Read, Write, Edit, Glob, Grep)
// Lines 401-500: Tool registry and definitions
// Lines 501-700: API client (request building, response parsing)
// Lines 701-850: Message management
// Lines 851-950: Main conversation loop
// Lines 951-1000: Entry point and cleanup
```

## License

This is a demonstration/educational implementation. Refer to Anthropic's terms of service for API usage.

## Contributing

This is a single-file implementation designed for simplicity. For production use, consider the official TypeScript version with full features.

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
- Startup: < 10ms
- API call: 1-5 seconds (depends on prompt complexity)
- Tool execution: Varies by tool
- Memory usage: ~5-10MB for typical conversation

## Security Notes

- API key read from environment (not stored in code)
- No sandboxing: Bash tool has full shell access
- File tools can access any readable/writable file
- Intended for trusted environments only

## Credits

Ported from the official Claude Code TypeScript implementation.
Simplified for educational purposes and embedded systems use.
