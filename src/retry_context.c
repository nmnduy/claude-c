#include "retry_context.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

#ifndef TESTING
#include "logger.h"
#else
// Mock logging for testing
#define LOG_DEBUG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) printf("[WARN] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) printf("[ERROR] " fmt "\n", ##__VA_ARGS__)
#endif

// Helper function to generate random jitter
static int random_jitter(int base_delay_ms, double percentage) {
    // Simple random number generation
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = true;
    }
    
    double jitter_range = base_delay_ms * percentage;
    int jitter = (int)(jitter_range * rand() / (RAND_MAX + 1.0));
    return jitter;
}

// Create a new retry context
RetryContext* retry_context_create(const RetryConfig *config) {
    if (!config) {
        return NULL;
    }
    
    RetryContext *ctx = malloc(sizeof(RetryContext));
    if (!ctx) {
        return NULL;
    }
    
    // Copy configuration
    ctx->config = *config;
    
    // Initialize state
    memset(&ctx->state, 0, sizeof(RetryState));
    ctx->state.last_error_message = NULL;
    
    return ctx;
}

// Destroy retry context and free resources
void retry_context_destroy(RetryContext *ctx) {
    if (!ctx) {
        return;
    }
    
    if (ctx->state.last_error_message) {
        free(ctx->state.last_error_message);
    }
    
    free(ctx);
}

// Reset retry state for reuse
void retry_context_reset(RetryContext *ctx) {
    if (!ctx) {
        return;
    }
    
    if (ctx->state.last_error_message) {
        free(ctx->state.last_error_message);
        ctx->state.last_error_message = NULL;
    }
    
    memset(&ctx->state, 0, sizeof(RetryState));
}

// Get default retry configuration
RetryConfig retry_config_default(void) {
    RetryConfig config = {
        .max_retries = RETRY_DEFAULT_MAX_RETRIES,
        .base_delay_ms = RETRY_DEFAULT_BASE_DELAY_MS,
        .max_delay_ms = RETRY_DEFAULT_MAX_DELAY_MS,
        .backoff_multiplier = RETRY_DEFAULT_BACKOFF_MULTIPLIER,
        .jitter_enabled = true,
        .retry_on_429 = true,
        .retry_on_5xx = true,
        .retry_on_timeout = true,
        .retry_on_connection_error = true
    };
    return config;
}

// Get aggressive retry configuration (more retries, shorter delays)
RetryConfig retry_config_aggressive(void) {
    RetryConfig config = {
        .max_retries = 8,
        .base_delay_ms = 500,
        .max_delay_ms = 15000,
        .backoff_multiplier = 1.5,
        .jitter_enabled = true,
        .retry_on_429 = true,
        .retry_on_5xx = true,
        .retry_on_timeout = true,
        .retry_on_connection_error = true
    };
    return config;
}

// Get conservative retry configuration (fewer retries, longer delays)
RetryConfig retry_config_conservative(void) {
    RetryConfig config = {
        .max_retries = 3,
        .base_delay_ms = 2000,
        .max_delay_ms = 60000,
        .backoff_multiplier = 3.0,
        .jitter_enabled = true,
        .retry_on_429 = true,
        .retry_on_5xx = false,  // Don't retry 5xx errors conservatively
        .retry_on_timeout = true,
        .retry_on_connection_error = true
    };
    return config;
}

// Check if HTTP status is retryable
bool is_retryable_http_status(int status) {
    switch (status) {
        case 429: // Rate limited
        case 500: // Internal server error
        case 502: // Bad gateway
        case 503: // Service unavailable
        case 504: // Gateway timeout
            return true;
        default:
            return false;
    }
}

// Check if error code is retryable (CURL codes)
bool is_retryable_error_code(int error_code) {
    switch (error_code) {
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_PARTIAL_FILE:
            return true;
        default:
            return false;
    }
}

// Calculate delay for next retry attempt
int calculate_delay_ms(const RetryContext *ctx) {
    if (!ctx) {
        return 0;
    }
    
    // Exponential backoff: base_delay * (multiplier ^ attempt_count)
    double delay = ctx->config.base_delay_ms * 
                   pow(ctx->config.backoff_multiplier, ctx->state.attempt_count);
    
    // Cap at maximum delay
    if (delay > ctx->config.max_delay_ms) {
        delay = ctx->config.max_delay_ms;
    }
    
    int delay_ms = (int)delay;
    
    // Add jitter if enabled
    if (ctx->config.jitter_enabled) {
        add_jitter(&delay_ms, ctx->config.base_delay_ms);
    }
    
    return delay_ms;
}

// Add random jitter to delay
void add_jitter(int *delay_ms, int base_delay_ms) {
    if (!delay_ms) {
        return;
    }
    
    int jitter = random_jitter(base_delay_ms, RETRY_JITTER_PERCENTAGE);
    *delay_ms += jitter;
    
    // Ensure we don't go negative
    if (*delay_ms < 0) {
        *delay_ms = 0;
    }
}

// Check if error indicates rate limiting
bool is_rate_limit_error(int http_status, const char *error_message) {
    if (http_status == 429) {
        return true;
    }
    
    // Check error message for rate limiting indicators
    if (error_message) {
        const char *rate_limit_indicators[] = {
            "rate limit",
            "too many requests",
            "quota exceeded",
            "throttled",
            "retry after"
        };
        
        char *lower_error = strdup(error_message);
        if (lower_error) {
            // Convert to lowercase for case-insensitive comparison
            for (char *p = lower_error; *p; p++) {
                *p = tolower(*p);
            }
            
            for (size_t i = 0; i < sizeof(rate_limit_indicators) / sizeof(rate_limit_indicators[0]); i++) {
                if (strstr(lower_error, rate_limit_indicators[i])) {
                    free(lower_error);
                    return true;
                }
            }
            
            free(lower_error);
        }
    }
    
    return false;
}

// Extract Retry-After header value from response headers
int extract_retry_after_seconds(const char *response_headers) {
    if (!response_headers) {
        return 0;
    }
    
    // Look for "Retry-After: N" header
    const char *retry_after = strstr(response_headers, "Retry-After:");
    if (!retry_after) {
        retry_after = strstr(response_headers, "retry-after:");
    }
    
    if (!retry_after) {
        return 0;
    }
    
    // Skip to the value
    const char *colon = strchr(retry_after, ':');
    if (!colon) {
        return 0;
    }
    
    const char *value = colon + 1;
    while (*value && isspace(*value)) {
        value++;
    }
    
    // Parse integer value
    char *endptr;
    long seconds = strtol(value, &endptr, 10);
    if (endptr == value || seconds < 0 || seconds > INT_MAX) {
        return 0;
    }
    
    return (int)seconds;
}

// Calculate appropriate delay for rate limiting
int calculate_rate_limit_delay(int http_status, const char *response_headers, const RetryConfig *config) {
    if (!config) {
        return 0;
    }
    
    // First check for Retry-After header
    int retry_after = extract_retry_after_seconds(response_headers);
    if (retry_after > 0) {
        // Convert seconds to milliseconds and add some buffer
        return (retry_after + 1) * 1000;
    }
    
    // If no Retry-After header, use exponential backoff starting from a higher base
    return config->base_delay_ms * 2; // Start with 2x base delay for rate limits
}

// Sleep for specified milliseconds
static void sleep_ms(int ms) {
    if (ms <= 0) {
        return;
    }
    
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    
    nanosleep(&ts, NULL);
}

// Log retry attempt
void retry_log_attempt(const RetryContext *ctx, const char *operation_name) {
    if (!ctx || !operation_name) {
        return;
    }
    
    LOG_DEBUG("Retry attempt %d/%d for %s", 
              ctx->state.attempt_count + 1, 
              ctx->config.max_retries + 1, 
              operation_name);
}

// Log retry failure
void retry_log_failure(const RetryContext *ctx, const char *operation_name, const char *error) {
    if (!ctx || !operation_name) {
        return;
    }
    
    LOG_WARN("Retry %d/%d failed for %s: %s", 
             ctx->state.attempt_count + 1, 
             ctx->config.max_retries + 1, 
             operation_name, 
             error ? error : "Unknown error");
}

// Log retry success
void retry_log_success(const RetryContext *ctx, const char *operation_name) {
    if (!ctx || !operation_name) {
        return;
    }
    
    LOG_INFO("Operation %s succeeded on attempt %d after %dms of delays", 
             operation_name, 
             ctx->state.attempt_count + 1,
             ctx->state.total_delay_ms);
}

// Core retry execution function
RetryResult retry_execute(RetryContext *ctx, RetryOperation operation, void *user_data) {
    if (!ctx || !operation) {
        return RETRY_FAILED_INVALID_ARGS;
    }
    
    const char *operation_name = "unknown_operation";
    
    // Initialize first attempt
    if (ctx->state.attempt_count == 0) {
        ctx->state.first_attempt_time = time(NULL);
        ctx->state.last_attempt_time = ctx->state.first_attempt_time;
    }
    
    while (ctx->state.attempt_count <= ctx->config.max_retries) {
        retry_log_attempt(ctx, operation_name);
        
        // Update attempt time
        ctx->state.last_attempt_time = time(NULL);
        
        // Clear previous error state
        if (ctx->state.last_error_message) {
            free(ctx->state.last_error_message);
            ctx->state.last_error_message = NULL;
        }
        ctx->state.last_http_status = 0;
        ctx->state.last_error_code = 0;
        
        // Execute the operation
        int result = operation(user_data, &ctx->state.last_http_status, &ctx->state.last_error_message);
        
        if (result == 0) { // Success
            retry_log_success(ctx, operation_name);
            return RETRY_SUCCESS;
        }
        
        // Operation failed, check if we should retry
        bool should_retry = false;
        int delay_ms = 0;
        
        // Check HTTP status retryability
        if (is_retryable_http_status(ctx->state.last_http_status)) {
            if (ctx->state.last_http_status == 429 && ctx->config.retry_on_429) {
                should_retry = true;
                delay_ms = calculate_rate_limit_delay(ctx->state.last_http_status, 
                                                      NULL, // Would need response headers from operation
                                                      &ctx->config);
            } else if (ctx->state.last_http_status >= 500 && ctx->state.last_http_status < 600 && 
                      ctx->config.retry_on_5xx) {
                should_retry = true;
                delay_ms = calculate_delay_ms(ctx);
            }
        }
        
        // Check error code retryability
        if (!should_retry && is_retryable_error_code(ctx->state.last_error_code)) {
            if ((ctx->state.last_error_code == CURLE_OPERATION_TIMEDOUT && ctx->config.retry_on_timeout) ||
                (ctx->state.last_error_code != CURLE_OPERATION_TIMEDOUT && ctx->config.retry_on_connection_error)) {
                should_retry = true;
                delay_ms = calculate_delay_ms(ctx);
            }
        }
        
        if (!should_retry) {
            // Non-retryable error
            retry_log_failure(ctx, operation_name, ctx->state.last_error_message);
            return RETRY_FAILED_PERMANENT;
        }
        
        // We have more retries available
        if (ctx->state.attempt_count < ctx->config.max_retries) {
            retry_log_failure(ctx, operation_name, ctx->state.last_error_message);
            
            // Sleep before retry
            if (delay_ms > 0) {
                LOG_DEBUG("Waiting %dms before retry", delay_ms);
                sleep_ms(delay_ms);
                ctx->state.total_delay_ms += delay_ms;
            }
            
            ctx->state.attempt_count++;
            continue;
        } else {
            // No more retries
            retry_log_failure(ctx, operation_name, "Maximum retries exceeded");
            return RETRY_FAILED_RETRYABLE;
        }
    }
    
    return RETRY_FAILED_RETRYABLE;
}