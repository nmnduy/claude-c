# Claude Code - Pure C Edition

A lightweight, modular implementation of a coding agent that interacts with an Open API compatible API. This is a pure C port of the core functionality from the TypeScript/Node.js Claude Code CLI.

![claude-c preview](assets/images/claude-c-preview.webp)

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

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for detailed system architecture, module structure, and component design.

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
export OPENAI_API_KEY="your-api-key-here"
```

### Environment Variables

**API Configuration:**
- `OPENAI_API_KEY` - Your Anthropic API key (required)
- `OPENAI_API_BASE` - Override API endpoint (default: https://api.anthropic.com/v1/messages)
- `OPENAI_MODEL` or `ANTHROPIC_MODEL` - Model to use (default: claude-sonnet-4-20250514)
- `DISABLE_PROMPT_CACHING` - Set to 1 to disable prompt caching

**Logging and Persistence:**
- `CLAUDE_C_LOG_PATH` - Full path to log file (e.g., `/var/log/claude.log`)
- `CLAUDE_C_LOG_DIR` - Directory for logs (will use `claude.log` filename)
- `CLAUDE_LOG_LEVEL` - Minimum log level: `DEBUG`, `INFO`, `WARN`, `ERROR` (default: INFO)
- `CLAUDE_C_DB_PATH` - Path to SQLite database for API call history (default: `~/.local/share/claude-c/api_calls.db`)

**UI Customization:**
- `CLAUDE_C_THEME` - Path to Kitty theme file (e.g., `./colorschemes/dracula.conf`)

**Example configuration:**
```bash
# API settings
export OPENAI_API_KEY="sk-ant-..."
export OPENAI_MODEL="claude-sonnet-4-20250514"

# Custom log and database locations
export CLAUDE_C_LOG_DIR="$HOME/logs/claude"
export CLAUDE_C_DB_PATH="$HOME/data/claude-api-calls.db"

# Enable debug logging
export CLAUDE_LOG_LEVEL="DEBUG"

# Use Dracula theme
export CLAUDE_C_THEME="./colorschemes/dracula.conf"
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

The TUI uses **Kitty terminal's theme format** - a simple, dependency-free configuration format. See [docs/COLOR_THEMES.md](docs/COLOR_THEMES.md) for detailed configuration options and available themes.

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

## Tools

The C implementation includes these core tools: Bash, Read, Write, Edit, Glob, and Grep. All tools support advanced features like regex patterns, line ranges, and multi-replace operations.



## Comparison to TypeScript Version

See [docs/COMPARISON.md](docs/COMPARISON.md) for a detailed comparison between the C implementation and the official TypeScript version, including feature parity, performance differences, and trade-offs.



## Future Enhancements

See [docs/FUTURE_ENHANCEMENTS.md](docs/FUTURE_ENHANCEMENTS.md) for the detailed roadmap, implementation plans, and estimated timeline for upcoming features.

## Code Structure

See [docs/CODE_STRUCTURE.md](docs/CODE_STRUCTURE.md) for detailed documentation of the modular architecture, including core modules, data structures, memory management patterns, and inter-module communication.

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



## Performance

Typical performance on modern hardware:
- CPU: barely registers
- Memory usage: ~10-20MB for typical conversation (modular architecture)

![Memory usage analysis](assets/images/claude-c-memory-usage.webp)

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
