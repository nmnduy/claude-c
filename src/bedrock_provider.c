/*
 * bedrock_provider.c - AWS Bedrock API provider implementation
 */

#define _POSIX_C_SOURCE 200809L

#include "bedrock_provider.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Bedrock Provider Implementation
// ============================================================================

/**
 * Build request URL for Bedrock provider
 * Bedrock endpoint already includes the full path with /invoke
 */
static char* bedrock_build_url(Provider *self, const char *base_url) {
    (void)base_url;  // Not used for Bedrock (we use config->endpoint)

    BedrockConfig *config = (BedrockConfig*)self->config;

    if (!config || !config->endpoint) {
        LOG_ERROR("Bedrock provider: endpoint is not configured");
        return NULL;
    }

    // Return a copy of the pre-configured endpoint
    // (Already includes the full path like https://bedrock-runtime.us-west-2.amazonaws.com/model/MODEL_ID/invoke)
    char *url = strdup(config->endpoint);
    if (!url) {
        LOG_ERROR("Bedrock provider: failed to duplicate endpoint URL");
        return NULL;
    }

    LOG_DEBUG("Bedrock provider: built URL: %s", url);
    return url;
}

/**
 * Setup headers with AWS SigV4 signing
 */
static struct curl_slist* bedrock_setup_headers(
    Provider *self,
    struct curl_slist *headers,
    const char *method,
    const char *url,
    const char *payload
) {
    BedrockConfig *config = (BedrockConfig*)self->config;

    if (!config || !config->creds) {
        LOG_ERROR("Bedrock provider: credentials are not configured");
        return NULL;
    }

    LOG_DEBUG("Bedrock provider: signing request with AWS SigV4...");

    // Use the existing bedrock_sign_request function from aws_bedrock.c
    headers = bedrock_sign_request(
        headers,
        method,
        url,
        payload,
        config->creds,
        config->region,
        AWS_BEDROCK_SERVICE
    );

    if (!headers) {
        LOG_ERROR("Bedrock provider: failed to sign request");
        return NULL;
    }

    LOG_DEBUG("Bedrock provider: request signed successfully");
    return headers;
}

/**
 * Format request from OpenAI format to Bedrock/Anthropic format
 */
static char* bedrock_format_request(Provider *self, const char *openai_json) {
    (void)self;  // Not needed for conversion

    if (!openai_json) {
        LOG_ERROR("Bedrock provider: input JSON is NULL");
        return NULL;
    }

    LOG_DEBUG("Bedrock provider: converting OpenAI format to Bedrock/Anthropic format...");

    // Use the existing bedrock_convert_request function from aws_bedrock.c
    char *bedrock_json = bedrock_convert_request(openai_json);
    if (!bedrock_json) {
        LOG_ERROR("Bedrock provider: failed to convert request format");
        return NULL;
    }

    LOG_DEBUG("Bedrock provider: request converted (size: %zu bytes)", strlen(bedrock_json));
    return bedrock_json;
}

/**
 * Parse response from Bedrock/Anthropic format to OpenAI format
 */
static cJSON* bedrock_parse_response(Provider *self, const char *response_body) {
    (void)self;  // Not needed for conversion

    if (!response_body) {
        LOG_ERROR("Bedrock provider: response body is NULL");
        return NULL;
    }

    LOG_DEBUG("Bedrock provider: converting Bedrock/Anthropic response to OpenAI format...");

    // Use the existing bedrock_convert_response function from aws_bedrock.c
    cJSON *openai_response = bedrock_convert_response(response_body);
    if (!openai_response) {
        LOG_ERROR("Bedrock provider: failed to convert response format");
        return NULL;
    }

    LOG_DEBUG("Bedrock provider: response converted successfully");
    return openai_response;
}

/**
 * Handle authentication errors and attempt credential refresh
 */
static int bedrock_provider_handle_auth_error(Provider *self, long http_status, const char *error_message, const char *response_body) {
    BedrockConfig *config = (BedrockConfig*)self->config;

    if (!config) {
        LOG_ERROR("Bedrock provider: config is NULL in auth error handler");
        return 0;
    }

    LOG_WARN("Bedrock provider: authentication error (HTTP %ld): %s",
             http_status, error_message ? error_message : "(no message)");

    // Use the existing bedrock_handle_auth_error function from aws_bedrock.c
    // This will attempt to refresh credentials if it's an auth error
    int refreshed = bedrock_handle_auth_error(config, http_status, error_message, response_body);

    if (refreshed) {
        LOG_INFO("Bedrock provider: credentials refreshed successfully, retry recommended");
    } else {
        LOG_WARN("Bedrock provider: could not refresh credentials or not an auth error");
    }

    return refreshed;
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
    provider->build_request_url = bedrock_build_url;
    provider->setup_headers = bedrock_setup_headers;
    provider->format_request = bedrock_format_request;
    provider->parse_response = bedrock_parse_response;
    provider->handle_auth_error = bedrock_provider_handle_auth_error;
    provider->cleanup = bedrock_cleanup;

    LOG_INFO("Bedrock provider created successfully (region: %s, model: %s)",
             config->region, config->model_id);
    return provider;
}
