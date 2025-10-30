/*
 * claude_internal.h - Internal API for Claude Code modules
 *
 * Shared types and functions used across modules
 */

#ifndef CLAUDE_INTERNAL_H
#define CLAUDE_INTERNAL_H

#include <cjson/cJSON.h>
#include "version.h"

// ============================================================================
// Configuration Constants
// ============================================================================

// Use centralized version from version.h
#define VERSION CLAUDE_C_VERSION

// API Configuration - defaults can be overridden by environment variables
// Note: For OpenAI, the provider will automatically append "/v1/chat/completions" if needed
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

// Provider is defined in provider.h
typedef struct Provider Provider;

// ============================================================================
// Enums
// ============================================================================

typedef enum {
    MSG_USER,
    MSG_ASSISTANT,
    MSG_SYSTEM
} MessageRole;

// ============================================================================
// Internal (Vendor-Agnostic) Content Types
// ============================================================================

/**
 * Internal content types - vendor-agnostic representation
 * These are converted to/from provider-specific formats (OpenAI, Anthropic, etc.)
 */
typedef enum {
    INTERNAL_TEXT,           // Plain text content
    INTERNAL_TOOL_CALL,      // Agent requesting tool execution
    INTERNAL_TOOL_RESPONSE   // Result from tool execution
} InternalContentType;

// ============================================================================
// Structs
// ============================================================================

/**
 * Internal content representation (vendor-agnostic)
 * Providers convert this to/from their specific API formats
 */
typedef struct {
    InternalContentType type;

    // For all types
    char *text;              // Plain text (for INTERNAL_TEXT) or NULL

    // For INTERNAL_TOOL_CALL and INTERNAL_TOOL_RESPONSE
    char *tool_id;           // Unique ID for this tool call/response
    char *tool_name;         // Tool name (e.g., "Bash", "Read", "Write")
    cJSON *tool_params;      // Tool parameters (for TOOL_CALL)
    cJSON *tool_output;      // Tool execution result (for TOOL_RESPONSE)
    int is_error;            // Whether tool execution failed (for TOOL_RESPONSE)
} InternalContent;

/**
 * Vendor-agnostic tool call representation
 * Extracted from provider-specific response formats
 */
typedef struct {
    char *id;                // Unique ID for this tool call
    char *name;              // Tool name (e.g., "Bash", "Read", "Write")
    cJSON *parameters;       // Tool parameters (owned by this struct, must be freed)
} ToolCall;

/**
 * Vendor-agnostic assistant message representation
 * Contains text content from the assistant's response
 */
typedef struct {
    char *text;              // Text content (may be NULL if only tools, owned by this struct)
} AssistantMessage;

/**
 * Vendor-agnostic API response
 * Returned by call_api() - contains parsed tools and assistant message
 */
typedef struct {
    AssistantMessage message;  // Assistant's text response
    ToolCall *tools;          // Array of tool calls (NULL if no tools)
    int tool_count;           // Number of tool calls
    cJSON *raw_response;      // Raw response for adding to history (owned, must be freed)
} ApiResponse;

/**
 * Internal message representation (vendor-agnostic)
 * Contains one or more content blocks
 */
typedef struct {
    MessageRole role;
    InternalContent *contents;
    int content_count;
} InternalMessage;

// ============================================================================
// Legacy Types (Deprecated - for backward compatibility during migration)
// ============================================================================

typedef enum {
    CONTENT_TEXT,
    CONTENT_TOOL_USE,
    CONTENT_TOOL_RESULT
} ContentType;

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
    InternalMessage messages[MAX_MESSAGES];  // Vendor-agnostic internal format
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

// Free all messages and their contents (including system message). Use at program shutdown.
void conversation_free(ConversationState *state);

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

/**
 * Free an ApiResponse structure and all its owned resources
 */
void api_response_free(ApiResponse *response);

/**
 * Get tool definitions for the API request
 * enable_caching: whether to add cache_control markers
 * Returns: cJSON array of tool definitions (caller must free)
 */
// Add cache_control marker to a content block
void add_cache_control(cJSON *obj);

// Get tool definitions for the API request
cJSON* get_tool_definitions(int enable_caching);

#endif // CLAUDE_INTERNAL_H
