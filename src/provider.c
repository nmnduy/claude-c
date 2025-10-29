/*
 * provider.c - API Provider initialization and selection
 */

#define _POSIX_C_SOURCE 200809L

#include "provider.h"
#include "openai_provider.h"
#include "bedrock_provider.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Default Anthropic API URL
#define DEFAULT_ANTHROPIC_URL "https://api.anthropic.com/v1/messages"

/**
 * Check if Bedrock mode is enabled via environment variable
 */
static int is_bedrock_enabled(void) {
    const char *use_bedrock = getenv("CLAUDE_CODE_USE_BEDROCK");
    return (use_bedrock && strcmp(use_bedrock, "1") == 0);
}

/**
 * Get API URL from environment, or return default
 */
static char* get_api_url_from_env(void) {
    const char *env_url = getenv("OPENAI_API_BASE");
    if (!env_url || env_url[0] == '\0') {
        env_url = getenv("ANTHROPIC_API_URL");
    }

    if (env_url && env_url[0] != '\0') {
        char *url = strdup(env_url);
        if (url) {
            LOG_INFO("Using API URL from environment: %s", url);
            return url;
        }
    }

    // Return default
    char *default_url = strdup(DEFAULT_ANTHROPIC_URL);
    if (default_url) {
        LOG_INFO("Using default API URL: %s", default_url);
    }
    return default_url;
}

/**
 * Initialize the appropriate provider based on environment configuration
 */
ProviderInitResult provider_init(const char *model, const char *api_key) {
    ProviderInitResult result = {NULL, NULL, NULL};

    LOG_DEBUG("Initializing provider (model: %s)...", model ? model : "(null)");

    if (!model || model[0] == '\0') {
        result.error_message = strdup("Model name is required");
        LOG_ERROR("Provider init failed: %s", result.error_message);
        return result;
    }

    // Check if Bedrock mode is enabled
    if (is_bedrock_enabled()) {
        LOG_INFO("Bedrock mode is enabled, creating Bedrock provider...");

        // Create Bedrock provider
        Provider *provider = bedrock_provider_create(model);
        if (!provider) {
            result.error_message = strdup("Failed to initialize Bedrock provider (check logs for details)");
            LOG_ERROR("Provider init failed: %s", result.error_message);
            return result;
        }

        // Get the Bedrock endpoint from the config
        BedrockConfig *config = (BedrockConfig*)provider->config;
        if (!config || !config->endpoint) {
            result.error_message = strdup("Bedrock provider initialized but endpoint is missing");
            LOG_ERROR("Provider init failed: %s", result.error_message);
            provider->cleanup(provider);
            return result;
        }

        // Return the endpoint as the base URL
        result.provider = provider;
        result.api_url = strdup(config->endpoint);
        if (!result.api_url) {
            result.error_message = strdup("Failed to allocate memory for Bedrock endpoint");
            LOG_ERROR("Provider init failed: %s", result.error_message);
            provider->cleanup(provider);
            return result;
        }

        LOG_INFO("Provider initialization successful: Bedrock (endpoint: %s)", result.api_url);
        return result;
    }

    // OpenAI mode (default)
    LOG_INFO("Using OpenAI-compatible provider...");

    if (!api_key || api_key[0] == '\0') {
        result.error_message = strdup("API key is required for OpenAI provider");
        LOG_ERROR("Provider init failed: %s", result.error_message);
        return result;
    }

    // Get base URL from environment or use default
    char *base_url = get_api_url_from_env();
    if (!base_url) {
        result.error_message = strdup("Failed to allocate memory for API URL");
        LOG_ERROR("Provider init failed: %s", result.error_message);
        return result;
    }

    // Create OpenAI provider
    Provider *provider = openai_provider_create(api_key, base_url);
    free(base_url);  // openai_provider_create makes its own copy

    if (!provider) {
        result.error_message = strdup("Failed to initialize OpenAI provider (check logs for details)");
        LOG_ERROR("Provider init failed: %s", result.error_message);
        return result;
    }

    // Get the base URL from the config
    OpenAIConfig *config = (OpenAIConfig*)provider->config;
    if (!config || !config->base_url) {
        result.error_message = strdup("OpenAI provider initialized but base URL is missing");
        LOG_ERROR("Provider init failed: %s", result.error_message);
        provider->cleanup(provider);
        return result;
    }

    result.provider = provider;
    result.api_url = strdup(config->base_url);
    if (!result.api_url) {
        result.error_message = strdup("Failed to allocate memory for API URL");
        LOG_ERROR("Provider init failed: %s", result.error_message);
        provider->cleanup(provider);
        return result;
    }

    LOG_INFO("Provider initialization successful: OpenAI (base URL: %s)", result.api_url);
    return result;
}
