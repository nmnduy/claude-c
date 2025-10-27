/*
 * claude_internal.h - Internal API for Claude Code modules
 *
 * Shared types and functions used across modules
 */

#ifndef CLAUDE_INTERNAL_H
#define CLAUDE_INTERNAL_H

#include <cjson/cJSON.h>

// ============================================================================
// Configuration Constants
// ============================================================================

#define VERSION "0.0.1"

// API Configuration - defaults can be overridden by environment variables
#define API_BASE_URL "https://api.openai.com"
#define DEFAULT_MODEL "o4-mini"
#define MAX_TOKENS 16384
#define MAX_TOOLS 10
#define BUFFER_SIZE 8192
#define MAX_MESSAGES 10000

// Retry configuration for rate limiting (429 errors)
#define MAX_RETRIES 3                    // Maximum number of retry attempts
#define INITIAL_BACKOFF_MS 1000          // Initial backoff delay in milliseconds
#define MAX_BACKOFF_MS 10000             // Maximum backoff delay in milliseconds (10 seconds)
#define BACKOFF_MULTIPLIER 2.0           // Exponential backoff multiplier

// ANSI color codes (for non-TUI output)
#define ANSI_RESET "\033[0m"
#define ANSI_BLUE "\033[34m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_RED "\033[31m"
#define ANSI_CYAN "\033[36m"

// ============================================================================
// Forward Declarations
// ============================================================================

// PersistenceDB is defined in persistence.h (or stubbed in TEST_BUILD)
struct PersistenceDB;

// TodoList is defined in todo.h
struct TodoList;

// ============================================================================
// Enums
// ============================================================================

typedef enum {
    MSG_USER,
    MSG_ASSISTANT,
    MSG_SYSTEM
} MessageRole;

typedef enum {
    CONTENT_TEXT,
    CONTENT_TOOL_USE,
    CONTENT_TOOL_RESULT
} ContentType;

// ============================================================================
// Structs
// ============================================================================

typedef struct {
    ContentType type;
    char *text;              // For TEXT
    char *tool_use_id;       // For TOOL_USE and TOOL_RESULT
    char *tool_name;         // For TOOL_USE
    cJSON *tool_input;       // For TOOL_USE
    cJSON *tool_result;      // For TOOL_RESULT
    int is_error;            // For TOOL_RESULT
} ContentBlock;

typedef struct {
    MessageRole role;
    ContentBlock *content;
    int content_count;
} Message;

typedef struct ConversationState {
    Message messages[MAX_MESSAGES];
    int count;
    char *api_key;
    char *api_url;
    char *model;
    char *working_dir;
    char **additional_dirs;         // Array of additional working directory paths
    int additional_dirs_count;      // Number of additional directories
    int additional_dirs_capacity;   // Capacity of additional_dirs array
    char *session_id;               // Unique session identifier for this conversation
    struct PersistenceDB *persistence_db;  // For logging API calls to SQLite
    struct TodoList *todo_list;     // Task tracking list
} ConversationState;

// ============================================================================
// Function Declarations
// ============================================================================

/**
 * Add a directory to the additional working directories list
 * Returns: 0 on success, -1 on error
 */
int add_directory(ConversationState *state, const char *path);

/**
 * Clear conversation history (keeps system message)
 */
void clear_conversation(ConversationState *state);

/**
 * Build system prompt with environment context
 * Returns: Newly allocated string (caller must free), or NULL on error
 */
char* build_system_prompt(ConversationState *state);

#endif // CLAUDE_INTERNAL_H
