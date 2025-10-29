/*
 * provider.h - API Provider abstraction layer
 *
 * Defines a common interface for different API providers (OpenAI, AWS Bedrock, etc.)
 * This abstraction separates provider-specific authentication, request formatting,
 * and error handling from the core conversation logic.
 */

#ifndef PROVIDER_H
#define PROVIDER_H

#include <curl/curl.h>
#include <cjson/cJSON.h>

// Forward declarations
struct Provider;
struct ConversationState;

/**
 * Result from a single API call attempt
 * Used by provider->call_api() to communicate success/error state to retry logic
 */
typedef struct {
    cJSON *response;         // Parsed response in OpenAI format (NULL on error, caller must delete)
    char *raw_response;      // Raw response body (for logging, caller must free)
    long http_status;        // HTTP status code (0 if network error before response)
    char *error_message;     // Error message if call failed (caller must free)
    long duration_ms;        // Request duration in milliseconds
    int is_retryable;        // 1 if error can be retried, 0 otherwise
    int auth_refreshed;      // 1 if provider refreshed credentials (AWS only)
} ApiCallResult;

/**
 * Provider interface - abstraction for API providers
 *
 * Each provider implements call_api() to handle a single authenticated request.
 * The generic retry logic wraps this to handle transient failures.
 */
typedef struct Provider {
    // Provider metadata
    const char *name;           // "OpenAI", "Bedrock", etc.
    void *config;               // Provider-specific configuration (opaque pointer)

    /**
     * Execute a single API call attempt (no retries)
     *
     * Provider-specific implementation that handles:
     * - Credential validation/refresh (AWS: check/refresh credentials before signing)
     * - Request formatting (OpenAI: pass-through, Bedrock: convert to Anthropic format)
     * - Authentication (OpenAI: Bearer token header, AWS: SigV4 request signing)
     * - HTTP execution (single attempt using libcurl)
     * - Response parsing (Bedrock: convert from Anthropic to OpenAI format)
     *
     * @param self - Provider instance
     * @param state - Conversation state with messages, model, etc.
     * @return ApiCallResult with response (on success) or error details (on failure)
     *         Caller must free result.response, result.raw_response, and result.error_message
     */
    ApiCallResult (*call_api)(struct Provider *self, struct ConversationState *state);

    /**
     * Cleanup provider resources
     *
     * @param self - Provider instance to cleanup (will be freed)
     */
    void (*cleanup)(struct Provider *self);

} Provider;

/**
 * Provider initialization result
 */
typedef struct {
    Provider *provider;   // Initialized provider (NULL on error)
    char *api_url;        // Base API URL for this provider
    char *error_message;  // Error message if initialization failed (caller must free)
} ProviderInitResult;

/**
 * Initialize the appropriate provider based on environment configuration
 *
 * Checks environment variables to determine which provider to use:
 * - CLAUDE_CODE_USE_BEDROCK=1 -> AWS Bedrock
 * - Otherwise -> OpenAI-compatible API
 *
 * @param model - Model name (e.g., "claude-sonnet-4-20250514")
 * @param api_key - API key (for OpenAI provider, may be NULL for Bedrock)
 * @return ProviderInitResult with provider and base URL, or error details
 *         Caller must free provider with provider->cleanup() and free api_url
 */
ProviderInitResult provider_init(const char *model, const char *api_key);

#endif // PROVIDER_H
