# Future Enhancements

## High Priority

- [ ] **Streaming API responses** (SSE) - Better UX
- [ ] **Session persistence** - Save/load conversations

## Medium Priority

- [ ] Configuration file support
- [ ] WebFetch tool with libcurl
- [ ] MCP (Model Context Protocol) server support
- [ ] Permission management system

## Low Priority

- [ ] Memory-efficient large file handling
- [ ] Comprehensive error recovery
- [ ] Hooks system
- [ ] Advanced tools (Task, etc.)

## Recently Completed ✅

- [x] **Modular architecture** - Clean separation into modules
- [x] **Advanced Edit tool** - Multi-replace and regex support
- [x] **Read tool enhancements** - Line range support
- [x] **Theme system** - Kitty-compatible terminal themes
- [x] **Logging system** - Comprehensive debugging support
- [x] **Persistence layer** - API history and session data
- [x] **Parallel tool execution** - Concurrent tool execution with pthreads
- [x] **ESC key interruption** - Stop agent work with ESC key during API calls or tool execution

## Implementation Roadmap

### Phase 1: Core UX Improvements
1. **Streaming API responses**
   - Implement SSE client using libcurl
   - Real-time response rendering
   - Better user feedback during long operations

2. **Session persistence**
   - Save conversation history to disk
   - Resume previous conversations
   - Conversation search and filtering

### Phase 2: Feature Parity
1. **Configuration system**
   - TOML/JSON config file support
   - Theme and preference persistence
   - Profile management

2. **Advanced tools**
   - WebFetch tool for HTTP requests
   - Task management tool
   - File system monitoring

### Phase 3: Advanced Features
1. **MCP Integration**
   - Model Context Protocol server support
   - External tool integration
   - Plugin architecture

2. **Permission Management**
   - Sandbox mode for file operations
   - Tool execution permissions
   - Security policies

## Technical Considerations

### Streaming Implementation
```c
// Planned SSE client structure
typedef struct {
    CURL *curl;
    char *buffer;
    size_t buffer_size;
    void (*on_chunk)(const char *data, size_t len, void *user_data);
    void *user_data;
} SSEClient;
```

### Session Storage Format
```c
// Planned conversation storage
typedef struct {
    char *session_id;
    time_t created_at;
    time_t updated_at;
    Message *messages;
    size_t message_count;
    char *title;  // Auto-generated or user-provided
} Session;
```

### Configuration Schema
```toml
[general]
theme = "dracula"
log_level = "info"
auto_save = true

[api]
model = "claude-sonnet-4-20250514"
max_tokens = 8192
timeout = 30

[tools]
parallel_execution = true
max_concurrent = 4

[security]
sandbox_mode = false
allowed_paths = [".", "~"]
```

## Contributing to Future Enhancements

Contributors are welcome to work on any of these items. Please:

1. Check existing issues for ongoing work
2. Create a feature request for major enhancements
3. Follow the coding standards in CLAUDE.md
4. Include tests for new functionality
5. Update documentation

## Estimated Timeline

Based on current development velocity:

- **Q1 2025**: Streaming responses and session persistence
- **Q2 2025**: Configuration system and WebFetch tool
- **Q3 2025**: MCP integration and permission management
- **Q4 2025**: Advanced features and performance optimizations

Note: Timeline is subject to change based on community contributions and project priorities.