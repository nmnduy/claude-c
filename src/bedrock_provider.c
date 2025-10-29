/*
 * bedrock_provider.c - AWS Bedrock API provider implementation
 */

#define _POSIX_C_SOURCE 200809L

#include "bedrock_provider.h"
#include "claude_internal.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

// ============================================================================
// Request Building (from ConversationState)
// ============================================================================

// Forward declaration - this will be implemented in claude.c and exposed via claude_internal.h
// For now, we declare it here as extern
extern char* build_request_json_from_state(ConversationState *state);

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
        // Success - parse response
        result.response = bedrock_convert_response(response.output);
        if (!result.response) {
            result.error_message = strdup("Failed to parse Bedrock response");
            result.is_retryable = 0;
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
                    LOG_INFO("Authentication successful, reloading credentials...");

                    // Reload credentials after successful authentication
                    AWSCredentials *new_creds = bedrock_load_credentials(profile, config->region);
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
                        LOG_ERROR("Failed to reload credentials after authentication");
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
                AWSCredentials *final_creds = bedrock_load_credentials(profile, config->region);
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
