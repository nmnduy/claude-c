/**
 * FormWork - C library for constructing structured data from LLM output
 *
 * A lightweight C port of the Java FormWork library, designed to:
 * - Extract JSON from LLM responses (handling markdown code blocks)
 * - Retry on parsing failures with error-correction prompts
 * - Track retry metrics
 * - Provide schema-based prompt building
 */

#ifndef FORMWORK_H
#define FORMWORK_H

#include <stddef.h>
#include <stdbool.h>
#include <cjson/cJSON.h>

// Default configuration values
#define FORMWORK_DEFAULT_MAX_RETRIES 3
#define FORMWORK_DEFAULT_RETRY_DELAY_MS 1000

// Error codes
typedef enum {
    FORMWORK_SUCCESS = 0,
    FORMWORK_ERROR_INVALID_JSON = -1,
    FORMWORK_ERROR_EMPTY_RESPONSE = -2,
    FORMWORK_ERROR_MAX_RETRIES = -3,
    FORMWORK_ERROR_CALLBACK_FAILED = -4,
    FORMWORK_ERROR_ALLOCATION_FAILED = -5,
    FORMWORK_ERROR_INVALID_CONFIG = -6
} FormWorkError;

// Forward declarations
typedef struct FormWorkConfig FormWorkConfig;
typedef struct FormWorkMetrics FormWorkMetrics;

/**
 * Function pointer type for LLM caller
 * @param prompt - The prompt string to send to the LLM
 * @param user_data - Optional user data passed through
 * @return Allocated string with LLM response (caller must free), or NULL on error
 */
typedef char* (*LLMCallerFunc)(const char *prompt, void *user_data);

/**
 * Function pointer type for error callback
 * @param error_code - The FormWork error code
 * @param error_msg - Human-readable error message
 * @param user_data - Optional user data passed through
 */
typedef void (*ErrorCallbackFunc)(FormWorkError error_code, const char *error_msg, void *user_data);

/**
 * Retry metrics callbacks - optional interface for monitoring retry behavior
 */
struct FormWorkMetrics {
    void (*on_attempt_start)(const char *target_name, int attempt, int max_retries, void *user_data);
    void (*on_attempt_success)(const char *target_name, int attempt, int max_retries, void *user_data);
    void (*on_attempt_retry)(const char *target_name, int attempt, int max_retries,
                             const char *error_msg, void *user_data);
    void (*on_final_failure)(const char *target_name, int total_attempts,
                             const char *error_msg, void *user_data);
    void *user_data;  // User data passed to all callbacks
};

/**
 * Configuration for FormWork construction
 */
struct FormWorkConfig {
    const char *target_name;           // Name of target structure (for logging)
    const char *base_prompt;           // Base prompt for LLM
    const char *json_schema;           // Optional JSON schema to include in prompt
    LLMCallerFunc llm_caller;          // Function to call LLM
    void *llm_user_data;               // User data for LLM caller
    int max_retries;                   // Maximum retry attempts (default: 3)
    long retry_delay_ms;               // Delay between retries in milliseconds (default: 1000)
    ErrorCallbackFunc error_callback;  // Optional error callback
    void *error_user_data;             // User data for error callback
    FormWorkMetrics *metrics;          // Optional retry metrics tracking
};

/**
 * Result of FormWork construction
 */
typedef struct {
    cJSON *json;                    // Parsed JSON object (caller owns, must call cJSON_Delete)
    FormWorkError error_code;       // Error code (0 = success)
    char *error_message;            // Error message (caller must free if non-NULL)
    int attempts_used;              // Number of attempts used
    char *last_llm_response;        // Last LLM response (for debugging, caller must free if non-NULL)
} FormWorkResult;

// ============================================================================
// Core API
// ============================================================================

/**
 * Initialize a FormWorkConfig with default values
 */
void formwork_config_init(FormWorkConfig *config);

/**
 * Build a full prompt with JSON schema instructions
 *
 * @param config - Configuration with target_name, base_prompt, and optional json_schema
 * @return Allocated string with full prompt (caller must free), or NULL on error
 */
char* formwork_build_prompt(const FormWorkConfig *config);

/**
 * Build a retry prompt with error correction instructions
 *
 * @param config - Configuration
 * @param last_error - Previous error message
 * @param last_response - Previous LLM response (optional, can be NULL)
 * @return Allocated string with retry prompt (caller must free), or NULL on error
 */
char* formwork_build_retry_prompt(const FormWorkConfig *config,
                                   const char *last_error,
                                   const char *last_response);

/**
 * Extract JSON from LLM output (handles markdown code blocks)
 *
 * Strips:
 * - Leading/trailing whitespace
 * - Markdown code fences (```json or ```)
 * - Text before first { or [
 * - Text after matching closing bracket
 *
 * @param llm_output - Raw LLM output string
 * @return Parsed cJSON object (caller must call cJSON_Delete), or NULL on error
 */
cJSON* formwork_extract_json(const char *llm_output);

/**
 * Construct a structured object from LLM with retry logic
 *
 * This is the main entry point. It will:
 * 1. Build the full prompt with schema
 * 2. Call the LLM
 * 3. Extract and parse JSON
 * 4. On failure, retry with error-correction prompt
 * 5. Track metrics if provided
 *
 * @param config - Configuration with all parameters
 * @return FormWorkResult with JSON or error (caller must call formwork_result_free)
 */
FormWorkResult formwork_construct(const FormWorkConfig *config);

/**
 * Free a FormWorkResult
 */
void formwork_result_free(FormWorkResult *result);

/**
 * Get human-readable error message for error code
 */
const char* formwork_error_string(FormWorkError error);

// ============================================================================
// Utility functions
// ============================================================================

/**
 * Simple JSON schema builder for common C structures
 *
 * This is a minimal helper - for complex schemas, generate externally and
 * pass via config->json_schema.
 *
 * @param type_name - Name of the type
 * @param fields - Array of field definitions (e.g., {"name", "string"}, {"age", "number"})
 * @param field_count - Number of fields
 * @return Allocated JSON schema string (caller must free), or NULL on error
 */
char* formwork_build_simple_schema(const char *type_name,
                                    const char *fields[][2],
                                    size_t field_count);

#endif // FORMWORK_H
