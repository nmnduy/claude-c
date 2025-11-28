/*
 * openai_provider.c - OpenAI-compatible API provider implementation
 */

#define _POSIX_C_SOURCE 200809L

#include "claude_internal.h"  // Must be first to get ApiResponse definition
#include "openai_provider.h"
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

// Progress callback placeholder (Ctrl+C handled by TUI)
static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)clientp;  // Unused
    (void)dltotal;  // Unused
    (void)dlnow;    // Unused
    (void)ultotal;  // Unused
    (void)ulnow;    // Unused

    // Interrupt (Ctrl+C) handling is done by the TUI event loop
    // Non-TUI mode doesn't have interactive interrupt support during API calls
    return 0;  // Continue transfer
}

// ============================================================================
// Request Building (using new message format)
// ============================================================================

#include "openai_messages.h"

// Helper to check if prompt caching is enabled
static int is_prompt_caching_enabled(void) {
    const char *disable_env = getenv("DISABLE_PROMPT_CACHING");
    return !(disable_env && (strcmp(disable_env, "1") == 0 ||
                             strcmp(disable_env, "true") == 0 ||
                             strcmp(disable_env, "TRUE") == 0));
}

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

    // Build request JSON using OpenAI message format
    int enable_caching = is_prompt_caching_enabled();
    cJSON *request = build_openai_request(state, enable_caching);
    if (!request) {
        result.error_message = strdup("Failed to build request JSON");
        result.is_retryable = 0;
        return result;
    }

    char *openai_json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    if (!openai_json) {
        result.error_message = strdup("Failed to serialize request JSON");
        result.is_retryable = 0;
        return result;
    }

    // Build full URL (base_url is already complete for OpenAI, just use it directly)
    // Actually, looking at the previous code, it needs /v1/chat/completions appended
    // But for Anthropic API, the base_url already includes the full path
    // For simplicity, just use base_url directly - it should be pre-configured correctly
    const char *url = config->base_url;

    // Set up headers
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // Add authentication header (custom format or default Bearer token)
    char auth_header[512];
    if (config->auth_header_template) {
        // Use custom auth header template (should contain %s for API key)
        // Find %s in the template and replace it with the API key
        const char *percent_s = strstr(config->auth_header_template, "%s");
        if (percent_s) {
            // Calculate lengths
            size_t prefix_len = (size_t)(percent_s - config->auth_header_template);
            size_t api_key_len = strlen(config->api_key);
            size_t suffix_len = strlen(percent_s + 2); // +2 to skip "%s"
            
            // Build auth header manually
            if (prefix_len + api_key_len + suffix_len + 1 < sizeof(auth_header)) {
                strncpy(auth_header, config->auth_header_template, prefix_len);
                auth_header[prefix_len] = '\0';
                strcat(auth_header, config->api_key);
                strcat(auth_header, percent_s + 2);
            } else {
                // Fallback if template is too long
                strncpy(auth_header, config->auth_header_template, sizeof(auth_header) - 1);
                auth_header[sizeof(auth_header) - 1] = '\0';
                LOG_WARN("Auth header template too long, truncated");
            }
        } else {
            // No %s found, use template as-is
            strncpy(auth_header, config->auth_header_template, sizeof(auth_header) - 1);
            auth_header[sizeof(auth_header) - 1] = '\0';
        }
    } else {
        // Default Bearer token format
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", config->api_key);
    }
    headers = curl_slist_append(headers, auth_header);

    // Add extra headers from environment
    if (config->extra_headers) {
        for (int i = 0; i < config->extra_headers_count; i++) {
            if (config->extra_headers[i]) {
                headers = curl_slist_append(headers, config->extra_headers[i]);
            }
        }
    }

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

    // Set timeouts to prevent indefinite hangs
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);  // 30 seconds to connect
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);         // 5 minutes total timeout

    // Enable progress callback (not used for Ctrl+C interrupts)
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);

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
        // Check if the error was due to user interruption (Ctrl+C)
        if (res == CURLE_ABORTED_BY_CALLBACK) {
            result.error_message = strdup("API call interrupted by user (Ctrl+C)");
            result.is_retryable = 0;  // User interruption is not retryable
        } else {
            result.error_message = strdup(curl_easy_strerror(res));
            result.is_retryable = (res == CURLE_COULDNT_CONNECT ||
                                   res == CURLE_OPERATION_TIMEDOUT ||
                                   res == CURLE_RECV_ERROR ||
                                   res == CURLE_SEND_ERROR ||
                                   res == CURLE_SSL_CONNECT_ERROR ||
                                   res == CURLE_GOT_NOTHING);
        }
        free(response.output);
        return result;
    }

    result.raw_response = response.output;

    // Check HTTP status
    if (result.http_status >= 200 && result.http_status < 300) {
        // Success - parse response (already in OpenAI format)
        cJSON *raw_json = cJSON_Parse(response.output);
        if (!raw_json) {
            result.error_message = strdup("Failed to parse JSON response");
            result.is_retryable = 0;
            return result;
        }

        // Extract vendor-agnostic response data
        ApiResponse *api_response = calloc(1, sizeof(ApiResponse));
        if (!api_response) {
            result.error_message = strdup("Failed to allocate ApiResponse");
            result.is_retryable = 0;
            cJSON_Delete(raw_json);
            return result;
        }

        // Initialize error_message to NULL
        api_response->error_message = NULL;

        // Keep raw response for history
        api_response->raw_response = raw_json;

        // Extract message from OpenAI response format
        cJSON *choices = cJSON_GetObjectItem(raw_json, "choices");
        if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
            result.error_message = strdup("Invalid response format: no choices");
            result.is_retryable = 0;
            api_response_free(api_response);
            return result;
        }

        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(choice, "message");
        if (!message) {
            result.error_message = strdup("Invalid response format: no message");
            result.is_retryable = 0;
            api_response_free(api_response);
            return result;
        }

        // Extract text content
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && cJSON_IsString(content) && content->valuestring) {
            api_response->message.text = strdup(content->valuestring);
        } else {
            api_response->message.text = NULL;
        }

        // Extract and validate tool calls
        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        if (tool_calls && cJSON_IsArray(tool_calls)) {
            int raw_tool_count = cJSON_GetArraySize(tool_calls);

            // First pass: count valid tool calls
            int valid_count = 0;
            for (int i = 0; i < raw_tool_count; i++) {
                cJSON *tool_call = cJSON_GetArrayItem(tool_calls, i);
                cJSON *function = cJSON_GetObjectItem(tool_call, "function");
                if (function) {
                    valid_count++;
                }
            }

            if (valid_count > 0) {
                api_response->tools = calloc((size_t)valid_count, sizeof(ToolCall));
                if (!api_response->tools) {
                    result.error_message = strdup("Failed to allocate tool calls");
                    result.is_retryable = 0;
                    api_response_free(api_response);
                    return result;
                }

                // Second pass: extract valid tool calls
                int tool_idx = 0;
                for (int i = 0; i < raw_tool_count; i++) {
                    cJSON *tool_call = cJSON_GetArrayItem(tool_calls, i);
                    cJSON *id = cJSON_GetObjectItem(tool_call, "id");
                    cJSON *function = cJSON_GetObjectItem(tool_call, "function");

                    if (!function) {
                        LOG_WARN("Skipping malformed tool_call at index %d (missing 'function' field)", i);
                        continue;
                    }

                    cJSON *name = cJSON_GetObjectItem(function, "name");
                    cJSON *arguments = cJSON_GetObjectItem(function, "arguments");

                    // Copy tool call data
                    api_response->tools[tool_idx].id =
                        (id && cJSON_IsString(id)) ? strdup(id->valuestring) : NULL;
                    api_response->tools[tool_idx].name =
                        (name && cJSON_IsString(name)) ? strdup(name->valuestring) : NULL;

                    // Parse arguments string to cJSON
                    if (arguments && cJSON_IsString(arguments)) {
                        api_response->tools[tool_idx].parameters = cJSON_Parse(arguments->valuestring);
                        if (!api_response->tools[tool_idx].parameters) {
                            LOG_WARN("Failed to parse tool arguments, using empty object");
                            api_response->tools[tool_idx].parameters = cJSON_CreateObject();
                        }
                    } else {
                        api_response->tools[tool_idx].parameters = cJSON_CreateObject();
                    }

                    tool_idx++;
                }
                api_response->tool_count = valid_count;
            }
        }

        result.response = api_response;
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
            cJSON *error_type = cJSON_GetObjectItem(error_obj, "type");

            // Check for context length limit error
            if (message && cJSON_IsString(message)) {
                const char *msg_text = message->valuestring;
                const char *type_text = (error_type && cJSON_IsString(error_type)) ? error_type->valuestring : "";

                // Detect context length overflow errors
                if ((strstr(msg_text, "maximum context length") != NULL) ||
                    (strstr(msg_text, "context length") != NULL && strstr(msg_text, "tokens") != NULL) ||
                    (strstr(msg_text, "too many tokens") != NULL) ||
                    (strcmp(type_text, "invalid_request_error") == 0 && strstr(msg_text, "tokens") != NULL)) {

                    // Provide user-friendly context length error message
                    result.error_message = strdup(
                        "Context length exceeded. The conversation has grown too large for the model's memory. "
                        "Try starting a new conversation or reduce the amount of code/files being discussed."
                    );
                    result.is_retryable = 0;  // Context length errors are not retryable
                } else {
                    // Use the original error message for other types of errors
                    result.error_message = strdup(msg_text);
                }
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
        free(config->auth_header_template);
        
        // Free extra headers
        if (config->extra_headers) {
            for (int i = 0; i < config->extra_headers_count; i++) {
                free(config->extra_headers[i]);
            }
            free(config->extra_headers);
        }
        
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
        // Check if base_url already has an endpoint path (contains "/v1/" or "/v2/" or "/v3/" or "/v4/" etc)
        // If it does, use it as-is; otherwise append the OpenAI endpoint
        if (strstr(base_url, "/v1/") != NULL || strstr(base_url, "/v2/") != NULL ||
            strstr(base_url, "/v3/") != NULL || strstr(base_url, "/v4/") != NULL) {
            // Already has an endpoint path (likely Anthropic, OpenAI, or custom)
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

    // Read custom auth header template from environment
    const char *auth_header_env = getenv("OPENAI_AUTH_HEADER");
    if (auth_header_env && auth_header_env[0] != '\0') {
        config->auth_header_template = strdup(auth_header_env);
        if (!config->auth_header_template) {
            LOG_ERROR("OpenAI provider: failed to duplicate auth header template");
            free(config->base_url);
            free(config->api_key);
            free(config);
            free(provider);
            return NULL;
        }
        LOG_INFO("OpenAI provider: using custom auth header template: %s", config->auth_header_template);
    } else {
        config->auth_header_template = NULL;  // Use default Bearer token format
    }

    // Read extra headers from environment
    const char *extra_headers_env = getenv("OPENAI_EXTRA_HEADERS");
    if (extra_headers_env && extra_headers_env[0] != '\0') {
        // Parse comma-separated headers
        char *extra_headers_copy = strdup(extra_headers_env);
        if (!extra_headers_copy) {
            LOG_ERROR("OpenAI provider: failed to duplicate extra headers string");
            free(config->auth_header_template);
            free(config->base_url);
            free(config->api_key);
            free(config);
            free(provider);
            return NULL;
        }

        // Count headers
        char *token = strtok(extra_headers_copy, ",");
        config->extra_headers_count = 0;
        while (token) {
            config->extra_headers_count++;
            token = strtok(NULL, ",");
        }

        // Allocate array
        config->extra_headers = calloc((size_t)config->extra_headers_count + 1, sizeof(char*));
        if (!config->extra_headers) {
            LOG_ERROR("OpenAI provider: failed to allocate extra headers array");
            free(extra_headers_copy);
            free(config->auth_header_template);
            free(config->base_url);
            free(config->api_key);
            free(config);
            free(provider);
            return NULL;
        }

        // Copy headers
        strcpy(extra_headers_copy, extra_headers_env);  // Reset copy
        token = strtok(extra_headers_copy, ",");
        for (int i = 0; i < config->extra_headers_count && token; i++) {
            // Trim whitespace
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token) - 1;
            while (end > token && (*end == ' ' || *end == '\t')) end--;
            *(end + 1) = '\0';
            
            config->extra_headers[i] = strdup(token);
            if (!config->extra_headers[i]) {
                LOG_ERROR("OpenAI provider: failed to duplicate extra header");
                // Cleanup allocated headers
                for (int j = 0; j < i; j++) {
                    free(config->extra_headers[j]);
                }
                free(config->extra_headers);
                free(extra_headers_copy);
                free(config->auth_header_template);
                free(config->base_url);
                free(config->api_key);
                free(config);
                free(provider);
                return NULL;
            }
            token = strtok(NULL, ",");
        }
        config->extra_headers[config->extra_headers_count] = NULL;  // NULL-terminate
        
        LOG_INFO("OpenAI provider: loaded %d extra headers", config->extra_headers_count);
        free(extra_headers_copy);
    } else {
        config->extra_headers = NULL;
        config->extra_headers_count = 0;
    }

    // Set up provider interface
    provider->name = "OpenAI";
    provider->config = config;
    provider->call_api = openai_call_api;
    provider->cleanup = openai_cleanup;

    LOG_INFO("OpenAI provider created successfully (base URL: %s)", config->base_url);
    return provider;
}
