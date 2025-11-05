/*
 * bedrock_provider.c - AWS Bedrock API provider implementation
 */

#define _POSIX_C_SOURCE 200809L

#include "claude_internal.h"  // Must be first to get ApiResponse definition
#include "bedrock_provider.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>  // for usleep
#include <curl/curl.h>

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

// Progress callback to check for ESC key during transfer
static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)clientp;  // Unused
    (void)dltotal;  // Unused
    (void)dlnow;    // Unused
    (void)ultotal;  // Unused
    (void)ulnow;    // Unused

    // ESC key handling is now done by TUI/ncurses event loop
    // Non-TUI mode doesn't have interactive ESC key support during API calls
    return 0;  // Continue transfer
}

// ============================================================================
// Request Building (from ConversationState)
// ============================================================================

// Forward declaration - this will be implemented in claude.c and exposed via claude_internal.h
// For now, we declare it here as extern


// ============================================================================
// Bedrock Provider Implementation
// ============================================================================

/**
 * Helper: Execute a single HTTP request with current credentials
 * Returns: ApiCallResult (caller must free fields)
 */
static ApiCallResult bedrock_execute_request(BedrockConfig *config, const char *bedrock_json) {
    ApiCallResult result = {0};

    // Sign request with SigV4 using current credentials
    struct curl_slist *headers = bedrock_sign_request(
        NULL, "POST", config->endpoint, bedrock_json,
        config->creds, config->region, AWS_BEDROCK_SERVICE
    );

    if (!headers) {
        result.error_message = strdup("Failed to sign request with AWS SigV4");
        result.is_retryable = 0;
        return result;
    }

    // Execute HTTP request
    CURL *curl = curl_easy_init();
    if (!curl) {
        result.error_message = strdup("Failed to initialize CURL");
        result.is_retryable = 0;
        curl_slist_free_all(headers);
        return result;
    }

    MemoryBuffer response = {NULL, 0};

    curl_easy_setopt(curl, CURLOPT_URL, config->endpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bedrock_json);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    // Enable progress callback for ESC interruption
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

    // Handle CURL errors
    if (res != CURLE_OK) {
        // Check if the error was due to user interruption (ESC key)
        if (res == CURLE_ABORTED_BY_CALLBACK) {
            result.error_message = strdup("API call interrupted by user (ESC)");
            result.is_retryable = 0;  // User interruption is not retryable
        } else {
            result.error_message = strdup(curl_easy_strerror(res));
            result.is_retryable = (res == CURLE_COULDNT_CONNECT ||
                                   res == CURLE_OPERATION_TIMEDOUT ||
                                   res == CURLE_RECV_ERROR ||
                                   res == CURLE_SEND_ERROR);
        }
        free(response.output);
        return result;
    }

    result.raw_response = response.output;

    // Check HTTP status
    if (result.http_status >= 200 && result.http_status < 300) {
        // Success - convert Bedrock response to OpenAI format
        cJSON *openai_json = bedrock_convert_response(response.output);
        if (!openai_json) {
            result.error_message = strdup("Failed to parse Bedrock response");
            result.is_retryable = 0;
            return result;
        }

        // Now extract vendor-agnostic response data (same as OpenAI provider)
        ApiResponse *api_response = calloc(1, sizeof(ApiResponse));
        if (!api_response) {
            result.error_message = strdup("Failed to allocate ApiResponse");
            result.is_retryable = 0;
            cJSON_Delete(openai_json);
            return result;
        }

        // Keep raw response for history
        api_response->raw_response = openai_json;

        // Extract message from OpenAI response format
        cJSON *choices = cJSON_GetObjectItem(openai_json, "choices");
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

        // Extract tool calls (same as OpenAI provider)
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
                    } else {
                        api_response->tools[tool_idx].parameters = NULL;
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
        cJSON *message = cJSON_GetObjectItem(error_json, "message");
        if (message && cJSON_IsString(message)) {
            result.error_message = strdup(message->valuestring);
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
 * Bedrock provider's call_api - handles AWS authentication with smart rotation detection
 */
static ApiCallResult bedrock_call_api(Provider *self, ConversationState *state) {
    ApiCallResult result = {0};
    BedrockConfig *config = (BedrockConfig*)self->config;

    if (!config || !config->creds) {
        result.error_message = strdup("Bedrock config or credentials not initialized");
        result.is_retryable = 0;
        return result;
    }

    // === STEP 1: Save initial token state (for external rotation detection) ===
    char *saved_access_key = NULL;
    if (config->creds->access_key_id) {
        saved_access_key = strdup(config->creds->access_key_id);
        LOG_DEBUG("Saved current access key ID for rotation detection: %.10s...", saved_access_key);
    }

    const char *profile = config->creds->profile ? config->creds->profile : "default";

    // === Build request (do this once, reuse for retries) ===
    char *openai_json = build_request_json_from_state(state);
    if (!openai_json) {
        result.error_message = strdup("Failed to build request JSON");
        result.is_retryable = 0;
        free(saved_access_key);
        return result;
    }

    char *bedrock_json = bedrock_convert_request(openai_json);
    free(openai_json);

    if (!bedrock_json) {
        result.error_message = strdup("Failed to convert request to Bedrock format");
        result.is_retryable = 0;
        free(saved_access_key);
        return result;
    }

    // === STEP 2: First API call attempt ===
    LOG_DEBUG("Executing first API call attempt...");
    result = bedrock_execute_request(config, bedrock_json);

    // Success on first try
    if (result.response) {
        LOG_INFO("API call succeeded on first attempt");
        free(saved_access_key);
        result.request_json = bedrock_json;  // Store for logging (caller frees)
        return result;
    }

    // === STEP 3: Auth error? Check for external credential rotation ===
    if (result.http_status == 401 || result.http_status == 403 || result.http_status == 400) {
        LOG_WARN("Authentication error (HTTP %ld): %s", result.http_status, result.error_message);
        LOG_DEBUG("=== CHECKING FOR EXTERNAL CREDENTIAL ROTATION ===");

        // Try loading fresh credentials from profile
        AWSCredentials *fresh_creds = bedrock_load_credentials(profile, config->region);

        if (fresh_creds) {
            LOG_DEBUG("Loaded fresh credentials from profile");

            // === STEP 4: Compare tokens - was it rotated externally? ===
            int externally_rotated = 0;
            if (saved_access_key && fresh_creds->access_key_id) {
                externally_rotated = (strcmp(saved_access_key, fresh_creds->access_key_id) != 0);
                LOG_DEBUG("Token comparison: saved=%.10s, fresh=%.10s, rotated=%s",
                         saved_access_key, fresh_creds->access_key_id,
                         externally_rotated ? "YES" : "NO");
            }

            if (externally_rotated) {
                // === STEP 4a: External rotation detected - use new credentials ===
                LOG_INFO("✓ Detected externally rotated credentials (another process updated tokens)");
                printf("\nDetected new AWS credentials from external source. Using updated credentials...\n");

                // Update config with externally rotated credentials
                bedrock_creds_free(config->creds);
                config->creds = fresh_creds;
                free(saved_access_key);
                saved_access_key = strdup(fresh_creds->access_key_id);
                result.auth_refreshed = 1;

                // Free previous error result
                free(result.raw_response);
                free(result.error_message);

                // === STEP 5: Retry with externally rotated credentials ===
                LOG_DEBUG("Retrying API call with externally rotated credentials...");
                result = bedrock_execute_request(config, bedrock_json);

                if (result.response) {
                    LOG_INFO("API call succeeded after using externally rotated credentials");
                    free(saved_access_key);
                    result.request_json = bedrock_json;  // Store for logging (caller frees)
                    return result;
                }

                LOG_WARN("API call still failed after external rotation: %s", result.error_message);
            } else {
                // === STEP 4b: No external rotation - force auth token rotation ===
                LOG_INFO("✗ Credentials unchanged, forcing authentication token rotation...");
                bedrock_creds_free(fresh_creds);

                // Call rotation command (aws sso login)
                LOG_DEBUG("Calling bedrock_authenticate to rotate credentials...");
                if (bedrock_authenticate(profile) == 0) {
                    LOG_INFO("Authentication successful, waiting for credential cache to update...");

                    // Poll for new credentials (AWS SSO writes credentials asynchronously)
                    // Try up to 10 times with 200ms intervals (max 2 seconds total)
                    AWSCredentials *new_creds = NULL;
                    int max_attempts = 10;
                    int attempt = 0;

                    for (attempt = 0; attempt < max_attempts; attempt++) {
                        if (attempt > 0) {
                            usleep(200000);  // 200ms between attempts
                        }

                        LOG_DEBUG("Polling for updated credentials (attempt %d/%d)...", attempt + 1, max_attempts);
                        AWSCredentials *polled_creds = bedrock_load_credentials(profile, config->region);

                        if (polled_creds && polled_creds->access_key_id) {
                            // Check if credentials have changed
                            if (saved_access_key && strcmp(saved_access_key, polled_creds->access_key_id) != 0) {
                                LOG_INFO("✓ Detected new credentials after rotation (attempt %d)", attempt + 1);
                                LOG_DEBUG("Old key: %.10s..., New key: %.10s...",
                                         saved_access_key, polled_creds->access_key_id);
                                new_creds = polled_creds;
                                break;
                            } else {
                                LOG_DEBUG("✗ Credentials unchanged (attempt %d)", attempt + 1);
                                bedrock_creds_free(polled_creds);
                            }
                        } else {
                            LOG_DEBUG("✗ Failed to load credentials (attempt %d)", attempt + 1);
                            if (polled_creds) bedrock_creds_free(polled_creds);
                        }
                    }

                    if (new_creds) {
                        bedrock_creds_free(config->creds);
                        config->creds = new_creds;
                        free(saved_access_key);
                        saved_access_key = strdup(new_creds->access_key_id);
                        result.auth_refreshed = 1;

                        // Free previous error result
                        free(result.raw_response);
                        free(result.error_message);

                        // === STEP 5: Retry with rotated credentials ===
                        LOG_DEBUG("Retrying API call with rotated credentials...");
                        result = bedrock_execute_request(config, bedrock_json);

                        if (result.response) {
                            LOG_INFO("API call succeeded after credential rotation");
                            free(saved_access_key);
                            result.request_json = bedrock_json;  // Store for logging (caller frees)
                            return result;
                        }

                        LOG_WARN("API call still failed after rotation: %s", result.error_message);
                    } else {
                        LOG_ERROR("Failed to detect new credentials after authentication (timed out after %d attempts)", max_attempts);
                    }
                } else {
                    LOG_ERROR("Authentication command failed");
                }
            }
        } else {
            LOG_ERROR("Failed to load fresh credentials from profile");
        }

        // === STEP 6: Still auth error? One final rotation attempt ===
        if ((result.http_status == 401 || result.http_status == 403 || result.http_status == 400) &&
            result.auth_refreshed) {
            LOG_WARN("Auth error persists after rotation, attempting one final rotation...");

            if (bedrock_authenticate(profile) == 0) {
                LOG_INFO("Final authentication successful, polling for updated credentials...");

                // Poll for new credentials with the same strategy
                AWSCredentials *final_creds = NULL;
                int max_attempts = 10;
                int attempt = 0;
                char *current_key = config->creds && config->creds->access_key_id ?
                                   strdup(config->creds->access_key_id) : NULL;

                for (attempt = 0; attempt < max_attempts; attempt++) {
                    if (attempt > 0) {
                        usleep(200000);  // 200ms between attempts
                    }

                    LOG_DEBUG("Polling for final credential update (attempt %d/%d)...", attempt + 1, max_attempts);
                    AWSCredentials *polled_creds = bedrock_load_credentials(profile, config->region);

                    if (polled_creds && polled_creds->access_key_id) {
                        // Check if credentials have changed
                        if (current_key && strcmp(current_key, polled_creds->access_key_id) != 0) {
                            LOG_INFO("✓ Detected new credentials on final rotation (attempt %d)", attempt + 1);
                            LOG_DEBUG("Old key: %.10s..., New key: %.10s...",
                                     current_key, polled_creds->access_key_id);
                            final_creds = polled_creds;
                            break;
                        } else {
                            LOG_DEBUG("✗ Credentials unchanged on final rotation (attempt %d)", attempt + 1);
                            bedrock_creds_free(polled_creds);
                        }
                    } else {
                        LOG_DEBUG("✗ Failed to load credentials on final rotation (attempt %d)", attempt + 1);
                        if (polled_creds) bedrock_creds_free(polled_creds);
                    }
                }

                free(current_key);

                if (final_creds) {
                    bedrock_creds_free(config->creds);
                    config->creds = final_creds;

                    // Free previous error result
                    free(result.raw_response);
                    free(result.error_message);

                    // === STEP 7: Final retry ===
                    LOG_DEBUG("Final API call attempt with re-rotated credentials...");
                    result = bedrock_execute_request(config, bedrock_json);

                    if (result.response) {
                        LOG_INFO("API call succeeded on final retry");
                    } else {
                        LOG_ERROR("API call failed even after final credential rotation");
                    }
                } else {
                    LOG_ERROR("Failed to detect new credentials on final rotation (timed out after %d attempts)", max_attempts);
                }
            }
        }
    }

    // Cleanup
    free(saved_access_key);
    result.request_json = bedrock_json;  // Store for logging even on error (caller frees)

    return result;
}

/**
 * Cleanup Bedrock provider resources
 */
static void bedrock_cleanup(Provider *self) {
    if (!self) return;

    LOG_DEBUG("Bedrock provider: cleaning up resources");

    if (self->config) {
        BedrockConfig *config = (BedrockConfig*)self->config;
        // Use the existing bedrock_config_free function from aws_bedrock.c
        bedrock_config_free(config);
    }

    free(self);
    LOG_DEBUG("Bedrock provider: cleanup complete");
}

// ============================================================================
// Public API
// ============================================================================

Provider* bedrock_provider_create(const char *model) {
    LOG_DEBUG("Creating Bedrock provider...");

    if (!model || model[0] == '\0') {
        LOG_ERROR("Bedrock provider: model name is required");
        return NULL;
    }

    // Initialize Bedrock configuration using existing function
    BedrockConfig *config = bedrock_config_init(model);
    if (!config) {
        LOG_ERROR("Bedrock provider: failed to initialize Bedrock configuration");
        return NULL;
    }

    // Allocate provider structure
    Provider *provider = calloc(1, sizeof(Provider));
    if (!provider) {
        LOG_ERROR("Bedrock provider: failed to allocate provider");
        bedrock_config_free(config);
        return NULL;
    }

    // Set up provider interface
    provider->name = "Bedrock";
    provider->config = config;
    provider->call_api = bedrock_call_api;
    provider->cleanup = bedrock_cleanup;

    LOG_INFO("Bedrock provider created successfully (region: %s, model: %s)",
             config->region, config->model_id);
    return provider;
}
