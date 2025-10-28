# Code Structure

## Core Modules

### claude.c (~300 lines)
- Main entry point and CLI argument parsing
- Conversation loop orchestration
- Environment context building

**Key functions:**
- `main()` - Entry point, argument parsing, initialization
- `run_conversation()` - Main conversation loop
- `build_system_prompt()` - Environment context assembly
- `cleanup()` - Resource cleanup and shutdown

### commands.c (~200 lines)
- Command-line argument processing
- Help system and validation
- CLI command dispatch

**Key functions:**
- `parse_arguments()` - Parse CLI arguments
- `show_help()` - Display usage information
- `validate_args()` - Argument validation

### tools/ (implemented in claude.c, ~400 lines total)

#### Bash Tool (~50 lines)
- Shell command execution via `popen()`
- Output capture and error handling
- Security considerations

#### Read Tool (~80 lines)
- File reading with line range support
- Efficient partial file reading
- Path resolution and validation

#### Write Tool (~60 lines)
- File content writing and creation
- Atomic write operations
- Directory creation if needed

#### Edit Tool (~120 lines)
- String replacement with regex support
- Multi-replace functionality
- POSIX Extended Regular Expressions

#### Glob Tool (~40 lines)
- Pattern matching with POSIX `glob()`
- Path expansion and filtering
- Error handling for invalid patterns

#### Grep Tool (~50 lines)
- Text search using system grep/ripgrep
- Recursive search options
- Output formatting

### api/ (implemented in claude.c, ~300 lines)
- HTTP client using libcurl
- JSON parsing with cJSON
- Message formatting and response handling

**Key functions:**
- `call_claude_api()` - Main API communication
- `build_request_json()` - Request formatting
- `parse_response()` - Response processing
- `handle_api_error()` - Error handling

### tui.c (~250 lines)
- Terminal UI framework
- Theme system with Kitty compatibility
- Color management and display

**Key functions:**
- `tui_init()` - Terminal initialization
- `tui_render_message()` - Message display
- `load_theme()` - Theme loading and parsing
- `tui_cleanup()` - Terminal restoration

### logger.c (~150 lines)
- Structured logging system
- Debug and audit trail support
- Configurable log levels

**Key functions:**
- `log_init()` - Logger initialization
- `log_message()` - Core logging function
- `log_set_level()` - Log level configuration
- `log_cleanup()` - Resource cleanup

### persistence.c (~200 lines)
- API call history storage
- Session data management
- Data migration utilities

**Key functions:**
- `persist_init()` - Storage initialization
- `save_conversation()` - Conversation persistence
- `load_conversation()` - Conversation loading
- `migrate_data()` - Schema migrations

### lineedit.c (~180 lines)
- Command line editing
- Auto-completion system
- Input handling

**Key functions:**
- `lineedit_init()` - Editor initialization
- `readline()` - Line input with editing
- `setup_completion()` - Completion system
- `handle_keybindings()` - Key event handling

### completion.c (~120 lines)
- Auto-completion engine
- Command and file completion
- Context-aware suggestions

**Key functions:**
- `completion_init()` - Completion system setup
- `generate_completions()` - Generate suggestions
- `complete_command()` - Command completion
- `complete_file()` - File path completion

### migrations.c (~100 lines)
- Data schema migrations
- Version compatibility handling
- Data transformation utilities

**Key functions:**
- `migrate_to_version()` - Version-specific migrations
- `check_schema_version()` - Schema validation
- `backup_data()` - Data backup before migration

## Data Structures

### Core Conversation Management
```c
typedef struct {
    char *role;        // "user", "assistant", "system"
    char *content;     // Message content
    time_t timestamp;  // Message timestamp
} Message;

typedef struct {
    Message *messages;
    size_t count;
    size_t capacity;
} Conversation;
```

### Tool System
```c
typedef struct {
    char *name;
    cJSON *parameters;
    char *result;
    int status;
} ToolCall;

typedef struct {
    ToolCall *calls;
    size_t count;
    size_t capacity;
} ToolExecution;
```

### Theme System
```c
typedef struct {
    RGB foreground;
    RGB background;
    RGB accent;
    RGB error;
    // ... more colors
} Theme;
```

### Logging System
```c
typedef struct {
    FILE *file;
    LogLevel level;
    bool console_output;
    bool file_output;
} Logger;
```

## Memory Management Patterns

### Initialization Pattern
```c
int module_init(ModuleConfig *config) {
    Module *module = malloc(sizeof(Module));
    if (!module) return -1;
    
    module->config = config;
    module->state = MODULE_READY;
    
    return 0;
}
```

### Cleanup Pattern
```c
void module_cleanup(Module *module) {
    if (!module) return;
    
    if (module->data) free(module->data);
    if (module->buffer) free(module->buffer);
    
    free(module);
}
```

### Error Handling Pattern
```c
int module_operation(Module *module, const char *input) {
    if (!module || !input) {
        return -1;  // Invalid arguments
    }
    
    if (module->state != MODULE_READY) {
        return -2;  // Module not ready
    }
    
    // ... operation logic ...
    
    return 0;  // Success
}
```

## Inter-Module Communication

### Event System
```c
typedef enum {
    EVENT_MESSAGE_RECEIVED,
    EVENT_TOOL_EXECUTED,
    EVENT_THEME_CHANGED,
    // ... more events
} EventType;

typedef struct {
    EventType type;
    void *data;
    time_t timestamp;
} Event;
```

### Callback Registration
```c
typedef void (*EventCallback)(Event *event);

int register_event_handler(EventType type, EventCallback callback);
void unregister_event_handler(EventType type, EventCallback callback);
void emit_event(Event *event);
```

## Build System Integration

### Module Compilation
```makefile
# Individual module compilation
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Linking all modules
claude-c: $(OBJECTS)
	$(CC) $(LDFLAGS) $(OBJECTS) -o $@
```

### Test Integration
```makefile
# Module-specific tests
test-%: tests/test_%.c src/%.o
	$(CC) $(TEST_CFLAGS) $^ -o $@
	./$@
```

## Total: ~2000+ lines of clean, modular C code

The modular architecture provides:
- **Maintainability**: Clear separation of concerns
- **Testability**: Each module can be tested independently
- **Extensibility**: New features can be added as modules
- **Reusability**: Modules can be reused in different contexts
- **Debugging**: Isolated modules simplify troubleshooting