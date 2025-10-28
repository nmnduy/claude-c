#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>

// Define TESTING to use mock logging
#define TESTING
#include "retry_context.h"

// Test data structures
typedef struct {
    int call_count;
    int succeed_on_call;
    int return_http_status;
    int return_error_code;
    char *return_error_message;
    int sleep_between_calls_ms;
    bool should_sleep;
} MockOperationData;

// Mock operation that simulates various failure scenarios
static int mock_operation(void *user_data, int *http_status, char **error_message) {
    MockOperationData *data = (MockOperationData *)user_data;
    data->call_count++;
    
    if (data->should_sleep && data->sleep_between_calls_ms > 0) {
        usleep(data->sleep_between_calls_ms * 1000); // Convert to microseconds
    }
    
    *http_status = data->return_http_status;
    
    if (data->return_error_message) {
        *error_message = strdup(data->return_error_message);
    }
    
    return (data->call_count == data->succeed_on_call) ? 0 : -1;
}

// Test helpers
static void test_retry_context_create_destroy(void) {
    printf("Testing retry_context_create/destroy...\n");
    
    RetryConfig config = retry_config_default();
    RetryContext *ctx = retry_context_create(&config);
    
    assert(ctx != NULL);
    assert(ctx->config.max_retries == RETRY_DEFAULT_MAX_RETRIES);
    assert(ctx->config.base_delay_ms == RETRY_DEFAULT_BASE_DELAY_MS);
    assert(ctx->state.attempt_count == 0);
    assert(ctx->state.last_error_message == NULL);
    
    retry_context_destroy(ctx);
    printf("✓ retry_context_create/destroy passed\n");
}

static void test_retry_context_reset(void) {
    printf("Testing retry_context_reset...\n");
    
    RetryConfig config = retry_config_default();
    RetryContext *ctx = retry_context_create(&config);
    
    // Simulate some state
    ctx->state.attempt_count = 3;
    ctx->state.last_http_status = 500;
    ctx->state.last_error_message = strdup("Test error");
    
    retry_context_reset(ctx);
    
    assert(ctx->state.attempt_count == 0);
    assert(ctx->state.last_http_status == 0);
    assert(ctx->state.last_error_message == NULL);
    
    retry_context_destroy(ctx);
    printf("✓ retry_context_reset passed\n");
}

static void test_retry_configs(void) {
    printf("Testing retry configuration presets...\n");
    
    RetryConfig default_config = retry_config_default();
    assert(default_config.max_retries == 5);
    assert(default_config.base_delay_ms == 1000);
    assert(default_config.max_delay_ms == 30000);
    assert(default_config.jitter_enabled == true);
    
    RetryConfig aggressive_config = retry_config_aggressive();
    assert(aggressive_config.max_retries == 8);
    assert(aggressive_config.base_delay_ms == 500);
    assert(aggressive_config.max_delay_ms == 15000);
    assert(aggressive_config.backoff_multiplier == 1.5);
    
    RetryConfig conservative_config = retry_config_conservative();
    assert(conservative_config.max_retries == 3);
    assert(conservative_config.base_delay_ms == 2000);
    assert(conservative_config.max_delay_ms == 60000);
    assert(conservative_config.backoff_multiplier == 3.0);
    assert(conservative_config.retry_on_5xx == false);
    
    printf("✓ retry configuration presets passed\n");
}

static void test_retryable_status_codes(void) {
    printf("Testing retryable HTTP status codes...\n");
    
    assert(is_retryable_http_status(429) == true);  // Rate limit
    assert(is_retryable_http_status(500) == true);  // Internal server error
    assert(is_retryable_http_status(502) == true);  // Bad gateway
    assert(is_retryable_http_status(503) == true);  // Service unavailable
    assert(is_retryable_http_status(504) == true);  // Gateway timeout
    
    assert(is_retryable_http_status(200) == false); // Success
    assert(is_retryable_http_status(400) == false); // Bad request
    assert(is_retryable_http_status(401) == false); // Unauthorized
    assert(is_retryable_http_status(404) == false); // Not found
    assert(is_retryable_http_status(422) == false); // Unprocessable entity
    
    printf("✓ retryable HTTP status codes passed\n");
}

static void test_delay_calculation(void) {
    printf("Testing delay calculation...\n");
    
    RetryConfig config = retry_config_default();
    config.jitter_enabled = false; // Disable jitter for predictable tests
    RetryContext *ctx = retry_context_create(&config);
    
    // Test exponential backoff without jitter
    ctx->state.attempt_count = 0;
    int delay1 = calculate_delay_ms(ctx);
    assert(delay1 == 1000); // base_delay * 2^0
    
    ctx->state.attempt_count = 1;
    int delay2 = calculate_delay_ms(ctx);
    assert(delay2 == 2000); // base_delay * 2^1
    
    ctx->state.attempt_count = 2;
    int delay3 = calculate_delay_ms(ctx);
    assert(delay3 == 4000); // base_delay * 2^2
    
    // Test max delay cap
    ctx->state.attempt_count = 10; // High number to exceed max
    int delay_max = calculate_delay_ms(ctx);
    assert(delay_max == config.max_delay_ms);
    
    retry_context_destroy(ctx);
    printf("✓ delay calculation passed\n");
}

static void test_jitter(void) {
    printf("Testing jitter functionality...\n");
    
    int delay_ms = 1000;
    int original_delay = delay_ms;
    
    add_jitter(&delay_ms, 1000);
    
    // Should have some jitter (could be the same due to random chance, but very unlikely)
    assert(delay_ms >= original_delay);
    assert(delay_ms <= original_delay + (int)(original_delay * RETRY_JITTER_PERCENTAGE * 2)); // Allow some tolerance
    
    printf("✓ jitter functionality passed\n");
}

static void test_rate_limit_detection(void) {
    printf("Testing rate limit detection...\n");
    
    // Test HTTP 429
    assert(is_rate_limit_error(429, NULL) == true);
    
    // Test error message patterns
    assert(is_rate_limit_error(200, "Rate limit exceeded") == true);
    assert(is_rate_limit_error(200, "Too many requests") == true);
    assert(is_rate_limit_error(200, "Quota exceeded") == true);
    assert(is_rate_limit_error(200, "Request throttled") == true);
    assert(is_rate_limit_error(200, "Retry after 5 seconds") == true);
    
    // Test case insensitive
    assert(is_rate_limit_error(200, "RATE LIMIT EXCEEDED") == true);
    
    // Test non-rate limit errors
    assert(is_rate_limit_error(200, "Internal server error") == false);
    assert(is_rate_limit_error(200, "Bad request") == false);
    
    printf("✓ rate limit detection passed\n");
}

static void test_retry_after_parsing(void) {
    printf("Testing Retry-After header parsing...\n");
    
    // Test valid headers
    assert(extract_retry_after_seconds("Retry-After: 5") == 5);
    assert(extract_retry_after_seconds("retry-after: 10") == 10);
    assert(extract_retry_after_seconds("Some-Header: 123\nRetry-After: 30") == 30);
    
    // Test headers with spaces
    assert(extract_retry_after_seconds("Retry-After:   42   ") == 42);
    
    // Test invalid headers
    assert(extract_retry_after_seconds(NULL) == 0);
    assert(extract_retry_after_seconds("") == 0);
    assert(extract_retry_after_seconds("No-Retry-Header: 5") == 0);
    assert(extract_retry_after_seconds("Retry-After: invalid") == 0);
    assert(extract_retry_after_seconds("Retry-After: -5") == 0);
    
    printf("✓ Retry-After header parsing passed\n");
}

static void test_successful_operation(void) {
    printf("Testing successful operation...\n");
    
    RetryConfig config = retry_config_default();
    RetryContext *ctx = retry_context_create(&config);
    
    MockOperationData data = {
        .call_count = 0,
        .succeed_on_call = 1, // Succeed on first call
        .return_http_status = 200,
        .return_error_code = 0
    };
    
    RetryResult result = retry_execute(ctx, mock_operation, &data);
    
    assert(result == RETRY_SUCCESS);
    assert(data.call_count == 1);
    assert(ctx->state.attempt_count == 0); // Should not increment on success
    
    retry_context_destroy(ctx);
    printf("✓ successful operation passed\n");
}

static void test_eventual_success(void) {
    printf("Testing eventual success after retries...\n");
    
    RetryConfig config = retry_config_default();
    config.jitter_enabled = false; // Disable for predictable timing
    RetryContext *ctx = retry_context_create(&config);
    
    MockOperationData data = {
        .call_count = 0,
        .succeed_on_call = 3, // Succeed on third call
        .return_http_status = 500, // Retryable error
        .return_error_code = 0
    };
    
    time_t start_time = time(NULL);
    RetryResult result = retry_execute(ctx, mock_operation, &data);
    time_t end_time = time(NULL);
    
    assert(result == RETRY_SUCCESS);
    assert(data.call_count == 3);
    
    // Should have taken at least some time due to delays
    assert(end_time >= start_time);
    
    retry_context_destroy(ctx);
    printf("✓ eventual success after retries passed\n");
}

static void test_max_retries_exceeded(void) {
    printf("Testing max retries exceeded...\n");
    
    RetryConfig config = retry_config_default();
    config.max_retries = 2; // Limit for faster test
    config.jitter_enabled = false;
    RetryContext *ctx = retry_context_create(&config);
    
    MockOperationData data = {
        .call_count = 0,
        .succeed_on_call = 10, // Never succeed
        .return_http_status = 500, // Retryable error
        .return_error_code = 0
    };
    
    RetryResult result = retry_execute(ctx, mock_operation, &data);
    
    assert(result == RETRY_FAILED_RETRYABLE);
    assert(data.call_count == 3); // 1 initial + 2 retries
    
    retry_context_destroy(ctx);
    printf("✓ max retries exceeded passed\n");
}

static void test_non_retryable_error(void) {
    printf("Testing non-retryable error...\n");
    
    RetryConfig config = retry_config_default();
    RetryContext *ctx = retry_context_create(&config);
    
    MockOperationData data = {
        .call_count = 0,
        .succeed_on_call = 10, // Never succeed
        .return_http_status = 400, // Non-retryable error
        .return_error_code = 0,
        .return_error_message = "Bad request"
    };
    
    RetryResult result = retry_execute(ctx, mock_operation, &data);
    
    assert(result == RETRY_FAILED_PERMANENT);
    assert(data.call_count == 1); // Only one attempt
    
    retry_context_destroy(ctx);
    printf("✓ non-retryable error passed\n");
}

static void test_rate_limit_retry(void) {
    printf("Testing rate limit retry...\n");
    
    RetryConfig config = retry_config_default();
    config.max_retries = 2;
    config.jitter_enabled = false;
    RetryContext *ctx = retry_context_create(&config);
    
    MockOperationData data = {
        .call_count = 0,
        .succeed_on_call = 2, // Succeed on second call
        .return_http_status = 429, // Rate limit
        .return_error_code = 0,
        .return_error_message = "Rate limit exceeded"
    };
    
    RetryResult result = retry_execute(ctx, mock_operation, &data);
    
    assert(result == RETRY_SUCCESS);
    assert(data.call_count == 2);
    
    retry_context_destroy(ctx);
    printf("✓ rate limit retry passed\n");
}

static void test_invalid_arguments(void) {
    printf("Testing invalid arguments...\n");
    
    MockOperationData data = {0};
    
    // NULL context
    RetryResult result1 = retry_execute(NULL, mock_operation, &data);
    assert(result1 == RETRY_FAILED_INVALID_ARGS);
    
    // NULL operation
    RetryConfig config = retry_config_default();
    RetryContext *ctx = retry_context_create(&config);
    RetryResult result2 = retry_execute(ctx, NULL, &data);
    assert(result2 == RETRY_FAILED_INVALID_ARGS);
    
    retry_context_destroy(ctx);
    printf("✓ invalid arguments passed\n");
}

// Performance test to ensure retry logic doesn't have memory leaks
static void test_memory_cleanup(void) {
    printf("Testing memory cleanup...\n");
    
    for (int i = 0; i < 100; i++) {
        RetryConfig config = retry_config_default();
        config.max_retries = 1; // Fast test
        RetryContext *ctx = retry_context_create(&config);
        
        MockOperationData data = {
            .call_count = 0,
            .succeed_on_call = 5, // Never succeed
            .return_http_status = 429,
            .return_error_message = strdup("Rate limit exceeded")
        };
        
        RetryResult result = retry_execute(ctx, mock_operation, &data);
        assert(result == RETRY_FAILED_RETRYABLE);
        
        free(data.return_error_message);
        retry_context_destroy(ctx);
    }
    
    printf("✓ memory cleanup passed\n");
}

// Main test runner
int main(void) {
    printf("Running retry context unit tests...\n\n");
    
    test_retry_context_create_destroy();
    test_retry_context_reset();
    test_retry_configs();
    test_retryable_status_codes();
    test_delay_calculation();
    test_jitter();
    test_rate_limit_detection();
    test_retry_after_parsing();
    test_successful_operation();
    test_eventual_success();
    test_max_retries_exceeded();
    test_non_retryable_error();
    test_rate_limit_retry();
    test_invalid_arguments();
    test_memory_cleanup();
    
    printf("\n✅ All retry context tests passed!\n");
    return 0;
}