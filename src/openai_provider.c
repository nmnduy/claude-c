/*
 * openai_provider.c - OpenAI-compatible API provider implementation
 */

#define _POSIX_C_SOURCE 200809L

#include "openai_provider.h"
#include "claude_internal.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <curl/curl.h>

// Default Anthropic API URL
#define DEFAULT_ANTHROPIC_URL "https://api.anthropic.com/v1/messages"

// ============================================================================
// CURL Helpers
// ============================================================================

typedef struct {
    char *output;
    size_t size;
} MemoryBuffer;

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemoryBuffer *mem = (MemoryBuffer *)userp;

    char *ptr = realloc(mem->output, mem->size + realsize + 1);
    if (!ptr) {
        LOG_ERROR("Not enough memory (realloc returned NULL)");
        return 0;
    }

    mem->output = ptr;
    memcpy(&(mem->output[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->output[mem->size] = 0;

    return realsize;
}

// ============================================================================
// Request Building (from ConversationState)
// ============================================================================

// Forward declaration - this will be implemented in claude.c and exposed via claude_internal.h
extern char* build_request_json_from_state(ConversationState *state);

// ============================================================================
// OpenAI Provider Implementation
// ============================================================================

/**
 * OpenAI provider's call_api - handles Bearer token authentication
 * Simple single-attempt API call with no auth rotation logic
 */
static ApiCallResult openai_call_api(Provider *self, ConversationState *state) {
    ApiCallResult result = {0};
    OpenAIConfig *config = (OpenAIConfig*)self->config;

    if (!config || !config->api_key || !config->base_url) {
        result.error_message = strdup("OpenAI config or credentials not initialized");
        result.is_retryable = 0;
        return result;
    }

    // Build request JSON from conversation state
    char *openai_json = build_request_json_from_state(state);
    if (!openai_json) {
        result.error_message = strdup("Failed to build request JSON");
        result.is_retryable = 0;
        return result;
    }

    // Build full URL (base_url is already complete for OpenAI, just use it directly)
    // Actually, looking at the previous code, it needs /v1/chat/completions appended
    // But for Anthropic API, the base_url already includes the full path
    // For simplicity, just use base_url directly - it should be pre-configured correctly
    const char *url = config->base_url;

    // Set up headers with Bearer token
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", config->api_key);
    headers = curl_slist_append(headers, auth_header);

    if (!headers) {
        result.error_message = strdup("Failed to setup HTTP headers");
        result.is_retryable = 0;
        result.request_json = openai_json;  // Store for logging
        return result;
    }

    // Execute HTTP request
    CURL *curl = curl_easy_init();
    if (!curl) {
        result.error_message = strdup("Failed to initialize CURL");
        result.is_retryable = 0;
        curl_slist_free_all(headers);
        result.request_json = openai_json;  // Store for logging
        return result;
    }

    MemoryBuffer response = {NULL, 0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, openai_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    CURLcode res = curl_easy_perform(curl);
    clock_gettime(CLOCK_MONOTONIC, &end);

    result.duration_ms = (end.tv_sec - start.tv_sec) * 1000 +
                         (end.tv_nsec - start.tv_nsec) / 1000000;

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // Store request JSON for logging (caller must free)
    result.request_json = openai_json;

    // Handle CURL errors
    if (res != CURLE_OK) {
        result.error_message = strdup(curl_easy_strerror(res));
        result.is_retryable = (res == CURLE_COULDNT_CONNECT ||
                               res == CURLE_OPERATION_TIMEDOUT ||
                               res == CURLE_RECV_ERROR ||
                               res == CURLE_SEND_ERROR);
        free(response.output);
        return result;
    }

    result.raw_response = response.output;

    // Check HTTP status
    if (result.http_status >= 200 && result.http_status < 300) {
        // Success - parse response (already in OpenAI format)
        result.response = cJSON_Parse(response.output);
        if (!result.response) {
            result.error_message = strdup("Failed to parse JSON response");
            result.is_retryable = 0;
            return result;
        }

        // Validate and sanitize tool_calls in the response
        // OpenAI format requires tool_calls to have a 'function' object
        cJSON *choices = cJSON_GetObjectItem(result.response, "choices");
        if (choices && cJSON_IsArray(choices)) {
            int choice_count = cJSON_GetArraySize(choices);
            for (int i = 0; i < choice_count; i++) {
                cJSON *choice = cJSON_GetArrayItem(choices, i);
                cJSON *message = cJSON_GetObjectItem(choice, "message");
                if (message) {
                    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                    if (tool_calls && cJSON_IsArray(tool_calls)) {
                        int tool_count = cJSON_GetArraySize(tool_calls);
                        // Check each tool_call for required 'function' field
                        for (int j = tool_count - 1; j >= 0; j--) {
                            cJSON *tool_call = cJSON_GetArrayItem(tool_calls, j);
                            cJSON *function = cJSON_GetObjectItem(tool_call, "function");
                            if (!function) {
                                // Remove malformed tool_call from array
                                LOG_WARN("Removing malformed tool_call at index %d (missing 'function' field)", j);
                                cJSON_DeleteItemFromArray(tool_calls, j);
                            }
                        }
                        // If all tool_calls were removed, delete the tool_calls array
                        if (cJSON_GetArraySize(tool_calls) == 0) {
                            cJSON_DeleteItemFromObject(message, "tool_calls");
                        }
                    }
                }
            }
        }

        return result;
    }

    // HTTP error
    result.is_retryable = (result.http_status == 429 ||
                           result.http_status == 408 ||
                           result.http_status >= 500);

    // Extract error message from response if JSON
    cJSON *error_json = cJSON_Parse(response.output);
    if (error_json) {
        cJSON *error_obj = cJSON_GetObjectItem(error_json, "error");
        if (error_obj) {
            cJSON *message = cJSON_GetObjectItem(error_obj, "message");
            if (message && cJSON_IsString(message)) {
                result.error_message = strdup(message->valuestring);
            }
        }
        cJSON_Delete(error_json);
    }

    if (!result.error_message) {
        char buf[256];
        snprintf(buf, sizeof(buf), "HTTP %ld", result.http_status);
        result.error_message = strdup(buf);
    }

    return result;
}

/**
 * Cleanup OpenAI provider resources
 */
static void openai_cleanup(Provider *self) {
    if (!self) return;

    LOG_DEBUG("OpenAI provider: cleaning up resources");

    if (self->config) {
        OpenAIConfig *config = (OpenAIConfig*)self->config;
        free(config->api_key);
        free(config->base_url);
        free(config);
    }

    free(self);
    LOG_DEBUG("OpenAI provider: cleanup complete");
}

// ============================================================================
// Public API
// ============================================================================

Provider* openai_provider_create(const char *api_key, const char *base_url) {
    LOG_DEBUG("Creating OpenAI provider...");

    if (!api_key || api_key[0] == '\0') {
        LOG_ERROR("OpenAI provider: API key is required");
        return NULL;
    }

    // Allocate provider structure
    Provider *provider = calloc(1, sizeof(Provider));
    if (!provider) {
        LOG_ERROR("OpenAI provider: failed to allocate provider");
        return NULL;
    }

    // Allocate config structure
    OpenAIConfig *config = calloc(1, sizeof(OpenAIConfig));
    if (!config) {
        LOG_ERROR("OpenAI provider: failed to allocate config");
        free(provider);
        return NULL;
    }

    // Copy API key
    config->api_key = strdup(api_key);
    if (!config->api_key) {
        LOG_ERROR("OpenAI provider: failed to duplicate API key");
        free(config);
        free(provider);
        return NULL;
    }

    // Copy or set default base URL - ensure it has the proper endpoint path
    if (base_url && base_url[0] != '\0') {
        // Check if base_url already has an endpoint path (contains "/v1/")
        // If it does, use it as-is; otherwise append the OpenAI endpoint
        if (strstr(base_url, "/v1/") != NULL) {
            // Already has an endpoint path (likely Anthropic or custom)
            config->base_url = strdup(base_url);
        } else {
            // Base domain only - append OpenAI chat completions endpoint
            size_t url_len = strlen(base_url) + strlen("/v1/chat/completions") + 1;
            config->base_url = malloc(url_len);
            if (config->base_url) {
                snprintf(config->base_url, url_len, "%s/v1/chat/completions", base_url);
                LOG_INFO("OpenAI provider: appended endpoint path to base URL: %s", config->base_url);
            }
        }

        if (!config->base_url) {
            LOG_ERROR("OpenAI provider: failed to set base URL");
            free(config->api_key);
            free(config);
            free(provider);
            return NULL;
        }
    } else {
        config->base_url = strdup(DEFAULT_ANTHROPIC_URL);
        if (!config->base_url) {
            LOG_ERROR("OpenAI provider: failed to set default base URL");
            free(config->api_key);
            free(config);
            free(provider);
            return NULL;
        }
    }

    // Set up provider interface
    provider->name = "OpenAI";
    provider->config = config;
    provider->call_api = openai_call_api;
    provider->cleanup = openai_cleanup;

    LOG_INFO("OpenAI provider created successfully (base URL: %s)", config->base_url);
    return provider;
}
