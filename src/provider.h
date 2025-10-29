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
 * Provider interface - abstraction for API providers
 *
 * Each provider implements these function pointers to handle:
 * - URL construction
 * - Authentication (headers, signing)
 * - Request/response format conversion
 * - Provider-specific error handling (e.g., credential refresh)
 */
typedef struct Provider {
    // Provider metadata
    const char *name;           // "OpenAI", "Bedrock", etc.
    void *config;               // Provider-specific configuration (opaque pointer)

    /**
     * Build the full API endpoint URL
     *
     * @param self - Provider instance
     * @param base_url - Base URL from configuration (may be provider-specific)
     * @return Newly allocated URL string (caller must free), or NULL on error
     */
    char* (*build_request_url)(struct Provider *self, const char *base_url);

    /**
     * Setup HTTP headers for authentication
     *
     * @param self - Provider instance
     * @param headers - Existing CURL header list (may be NULL)
     * @param method - HTTP method (e.g., "POST")
     * @param url - Full request URL
     * @param payload - Request body (for signing)
     * @return Updated header list, or NULL on error
     */
    struct curl_slist* (*setup_headers)(
        struct Provider *self,
        struct curl_slist *headers,
        const char *method,
        const char *url,
        const char *payload
    );

    /**
     * Format request from internal format to provider-specific format
     *
     * @param self - Provider instance
     * @param openai_json - Request in OpenAI format (internal representation)
     * @return Newly allocated JSON string in provider format (caller must free), or NULL on error
     *         For OpenAI provider, this is typically a no-op (returns copy of input)
     *         For Bedrock, this converts OpenAI format to Anthropic Messages API format
     */
    char* (*format_request)(struct Provider *self, const char *openai_json);

    /**
     * Parse provider response into standard format
     *
     * @param self - Provider instance
     * @param response_body - Raw response body from API
     * @return cJSON object in OpenAI format (caller must delete), or NULL on error
     *         For OpenAI provider, this is typically direct parsing
     *         For Bedrock, this converts Anthropic format back to OpenAI format
     */
    cJSON* (*parse_response)(struct Provider *self, const char *response_body);

    /**
     * Handle authentication errors and attempt recovery
     *
     * This is called when an API request fails with an auth-related error.
     * The provider can attempt to refresh credentials, re-authenticate, etc.
     *
     * @param self - Provider instance (config may be updated with new credentials)
     * @param http_status - HTTP status code from failed request
     * @param error_message - Error message from API response
     * @return 1 if credentials were refreshed (caller should retry), 0 otherwise
     */
    int (*handle_auth_error)(struct Provider *self, long http_status, const char *error_message);

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
