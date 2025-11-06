# Claude Code - Pure C Edition

A lightweight, modular implementation of a coding agent that interacts with an Open API compatible API. This is a pure C port of the core functionality from the TypeScript/Node.js Claude Code CLI.

![claude-c preview](assets/images/claude-c-preview.webp)

## Features

- **Modular architecture**: Clean separation into focused modules (~2000+ lines total)
- **Core tools**: Bash, Read, Write, Edit, Glob, Grep with advanced features
- **Voice input**: Record and transcribe audio using OpenAI Whisper API
- **Vendor support**: OpenAI and AWS Bedrock
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
- **portaudio**: Cross-platform audio recording (optional, for voice input)
- **POSIX**: File operations, glob, process management

### Installing Dependencies

**macOS (Homebrew):**
```bash
brew install curl cjson portaudio
```

**Ubuntu/Debian:**
```bash
sudo apt-get install libcurl4-openssl-dev libcjson-dev portaudio19-dev build-essential
```

**Fedora/RHEL:**
```bash
sudo yum install libcurl-devel cjson-devel portaudio-devel gcc make
```

## Building

**Recommended: Use stable release**
```bash
# Clone the latest stable(ish) version (v0.0.17)
git clone --branch v0.0.18 https://github.com/nmnduy/claude-c.git
cd claude-c
make
```

**Building from source (latest development):**
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

### Quick start

```sh
export OPENAI_API_KEY="$OPENROUTER_API_KEY"
export OPENAI_API_BASE="https://openrouter.ai/api"
export OPENAI_MODEL="z-ai/glm-4.6"
claude-c
```
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
- `CLAUDE_C_DB_PATH` - Path to SQLite database for API call history (default: `./.claude-c/api_calls.db`)

**Database Rotation:**
- `CLAUDE_C_DB_MAX_DAYS` - Keep records for N days (default: 30, 0=unlimited)
- `CLAUDE_C_DB_MAX_RECORDS` - Keep last N records (default: 1000, 0=unlimited)
- `CLAUDE_C_DB_MAX_SIZE_MB` - Max database size in MB (default: 100, 0=unlimited)
- `CLAUDE_C_DB_AUTO_ROTATE` - Enable auto-rotation on startup (default: 1, set to 0 to disable)

**Default Locations:**
By default, logs and API call history are stored in `./.claude-c/` in the current working directory. This makes each project self-contained. Override with the environment variables above if needed.

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
- **ESC key interruption**: Press ESC to stop the agent during API calls or tool execution
- **Normal mode navigation**: Press `Ctrl+G` to enter Normal mode for Vim-like scrolling (j/k, Ctrl+D/U, gg/G), press `i` to return to Insert mode
- Slash commands: `/clear`, `/exit`, `/quit`, `/help`, `/add-dir`, `/voice`
- Voice input: Use `/voice` to record and transcribe audio (requires PortAudio and OPENAI_API_KEY)
- Readline-style editing: Ctrl+A, Ctrl+E, Alt+B, Alt+F, etc.

### Color Theme Support

The TUI uses **Kitty terminal's theme format** - a simple, dependency-free configuration format. See [docs/COLOR_THEMES.md](docs/COLOR_THEMES.md) for detailed configuration options and available themes.

## Memory footprint

![Memory usage analysis](assets/images/claude-c-memory-usage.webp)

## Known Issues

- [ ] `SIGSTP` then resume will stop the app

## Security Notes

- API key read from environment (not stored in code)
- No sandboxing: Bash tool has full shell access
- File tools can access any readable/writable file
- Intended for trusted environments only
