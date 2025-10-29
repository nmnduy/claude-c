/*
 * claude_internal.h - Internal API for Claude Code modules
 *
 * Shared types and functions used across modules
 */

#ifndef CLAUDE_INTERNAL_H
#define CLAUDE_INTERNAL_H

#include <cjson/cJSON.h>
#include "version.h"
#include "provider.h"

// ============================================================================
// Configuration Constants
// ============================================================================

// Use centralized version from version.h
#define VERSION CLAUDE_C_VERSION

// API Configuration - defaults can be overridden by environment variables
#define API_BASE_URL "https://api.openai.com"
#define DEFAULT_MODEL "o4-mini"
#define MAX_TOKENS 16384
#define MAX_TOOLS 10
#define BUFFER_SIZE 8192
#define MAX_MESSAGES 10000

// Retry configuration for rate limiting (429 errors)
#define MAX_RETRY_DURATION_MS 120000     // Maximum retry duration (2 minutes)
#define INITIAL_BACKOFF_MS 1000          // Initial backoff delay in milliseconds
#define MAX_BACKOFF_MS 60000             // Maximum backoff delay in milliseconds (60 seconds)
#define BACKOFF_MULTIPLIER 2.0           // Exponential backoff multiplier

// ============================================================================
// Forward Declarations
// ============================================================================

// PersistenceDB is defined in persistence.h (or stubbed in TEST_BUILD)
struct PersistenceDB;

// TodoList is defined in todo.h
struct TodoList;

// BedrockConfig is defined in aws_bedrock.h (opaque pointer)
struct BedrockConfigStruct;

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
    Provider *provider;             // API provider abstraction (OpenAI, Bedrock, etc.)
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

/**
 * Build request JSON from conversation state (in OpenAI format)
 * Used by providers to get the request body with messages, tools, and cache markers
 * Returns: Newly allocated JSON string (caller must free), or NULL on error
 */
char* build_request_json_from_state(ConversationState *state);

#endif // CLAUDE_INTERNAL_H
