/*
 * openai_provider.c - OpenAI-compatible API provider implementation
 */

#define _POSIX_C_SOURCE 200809L

#include "openai_provider.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default Anthropic API URL
#define DEFAULT_ANTHROPIC_URL "https://api.anthropic.com/v1/messages"

// ============================================================================
// OpenAI Provider Implementation
// ============================================================================

/**
 * Build request URL for OpenAI provider
 * Appends /v1/chat/completions to the base URL
 */
static char* openai_build_url(Provider *self, const char *base_url) {
    (void)self;  // Provider context not needed for OpenAI

    if (!base_url || base_url[0] == '\0') {
        LOG_ERROR("OpenAI provider: base_url is NULL or empty");
        return NULL;
    }

    // Construct full URL: base_url + /v1/chat/completions
    size_t url_len = strlen(base_url) + strlen("/v1/chat/completions") + 1;
    char *full_url = malloc(url_len);
    if (!full_url) {
        LOG_ERROR("OpenAI provider: malloc failed for URL");
        return NULL;
    }

    snprintf(full_url, url_len, "%s/v1/chat/completions", base_url);
    LOG_DEBUG("OpenAI provider: built URL: %s", full_url);
    return full_url;
}

/**
 * Setup headers with Bearer token authentication
 */
static struct curl_slist* openai_setup_headers(
    Provider *self,
    struct curl_slist *headers,
    const char *method,
    const char *url,
    const char *payload
) {
    (void)method;   // Not needed for Bearer auth
    (void)url;      // Not needed for Bearer auth
    (void)payload;  // Not needed for Bearer auth

    OpenAIConfig *config = (OpenAIConfig*)self->config;

    if (!config || !config->api_key) {
        LOG_ERROR("OpenAI provider: API key is missing");
        return NULL;
    }

    // Add Content-Type header
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!headers) {
        LOG_ERROR("OpenAI provider: failed to append Content-Type header");
        return NULL;
    }

    // Add Authorization header with Bearer token
    char auth_header[512];
    int ret = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", config->api_key);
    if (ret < 0 || (size_t)ret >= sizeof(auth_header)) {
        LOG_ERROR("OpenAI provider: failed to format Authorization header");
        curl_slist_free_all(headers);
        return NULL;
    }

    headers = curl_slist_append(headers, auth_header);
    if (!headers) {
        LOG_ERROR("OpenAI provider: failed to append Authorization header");
        return NULL;
    }

    LOG_DEBUG("OpenAI provider: headers configured with Bearer token");
    return headers;
}

/**
 * Format request - for OpenAI, this is a no-op (returns copy)
 * The request is already in OpenAI format
 */
static char* openai_format_request(Provider *self, const char *openai_json) {
    (void)self;  // Not needed

    if (!openai_json) {
        LOG_ERROR("OpenAI provider: input JSON is NULL");
        return NULL;
    }

    // Return a copy of the input (already in OpenAI format)
    char *copy = strdup(openai_json);
    if (!copy) {
        LOG_ERROR("OpenAI provider: strdup failed for request JSON");
        return NULL;
    }

    LOG_DEBUG("OpenAI provider: request formatted (pass-through, size: %zu bytes)", strlen(copy));
    return copy;
}

/**
 * Parse response - direct JSON parsing (already in OpenAI format)
 */
static cJSON* openai_parse_response(Provider *self, const char *response_body) {
    (void)self;  // Not needed

    if (!response_body) {
        LOG_ERROR("OpenAI provider: response body is NULL");
        return NULL;
    }

    cJSON *response = cJSON_Parse(response_body);
    if (!response) {
        LOG_ERROR("OpenAI provider: failed to parse JSON response");
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr) {
            LOG_ERROR("OpenAI provider: JSON parse error near: %.50s", error_ptr);
        }
        return NULL;
    }

    LOG_DEBUG("OpenAI provider: response parsed successfully");
    return response;
}

/**
 * Handle authentication errors
 * For OpenAI, we don't have automatic credential refresh, so just log and return 0
 */
static int openai_handle_auth_error(Provider *self, long http_status, const char *error_message, const char *response_body) {
    (void)self;  // No credential refresh for OpenAI
    (void)response_body;  // Not used for OpenAI

    LOG_WARN("OpenAI provider: authentication error (HTTP %ld): %s",
             http_status, error_message ? error_message : "(no message)");
    LOG_WARN("OpenAI provider: please check your API key and try again");

    // Return 0 (no retry) - OpenAI doesn't support automatic credential refresh
    return 0;
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

    // Copy or set default base URL
    if (base_url && base_url[0] != '\0') {
        config->base_url = strdup(base_url);
        if (!config->base_url) {
            LOG_ERROR("OpenAI provider: failed to duplicate base URL");
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
    provider->build_request_url = openai_build_url;
    provider->setup_headers = openai_setup_headers;
    provider->format_request = openai_format_request;
    provider->parse_response = openai_parse_response;
    provider->handle_auth_error = openai_handle_auth_error;
    provider->cleanup = openai_cleanup;

    LOG_INFO("OpenAI provider created successfully (base URL: %s)", config->base_url);
    return provider;
}
