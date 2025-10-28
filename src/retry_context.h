#ifndef RETRY_CONTEXT_H
#define RETRY_CONTEXT_H

#include <time.h>
#include <stdbool.h>

// Retry configuration structure
typedef struct {
    int max_retries;           // Maximum number of retry attempts (default: 5)
    int base_delay_ms;         // Base delay in milliseconds (default: 1000)
    int max_delay_ms;          // Maximum delay in milliseconds (default: 30000)
    double backoff_multiplier; // Exponential backoff multiplier (default: 2.0)
    bool jitter_enabled;       // Add random jitter to delays (default: true)
    bool retry_on_429;         // Retry on HTTP 429 (rate limit) (default: true)
    bool retry_on_5xx;         // Retry on HTTP 5xx errors (default: true)
    bool retry_on_timeout;     // Retry on network timeouts (default: true)
    bool retry_on_connection_error; // Retry on connection failures (default: true)
} RetryConfig;

// Retry state for tracking a single operation
typedef struct {
    int attempt_count;         // Current attempt number (0-based)
    time_t first_attempt_time; // When the first attempt was made
    time_t last_attempt_time;  // When the last attempt was made
    int last_http_status;      // Last HTTP status code received
    int last_error_code;       // Last error code (CURLcode, etc.)
    char *last_error_message;  // Last error message
    int total_delay_ms;        // Total time spent in delays so far
} RetryState;

// Retry context combining config and state
typedef struct {
    RetryConfig config;
    RetryState state;
} RetryContext;

// Function pointer type for the operation to retry
typedef int (*RetryOperation)(void *user_data, int *http_status, char **error_message);

// Retry result codes
typedef enum {
    RETRY_SUCCESS = 0,         // Operation succeeded
    RETRY_FAILED_PERMANENT,    // Operation failed, no more retries
    RETRY_FAILED_RETRYABLE,    // Operation failed but could be retried (max retries exceeded)
    RETRY_FAILED_INVALID_ARGS  // Invalid arguments passed
} RetryResult;

// Retry context management functions
RetryContext* retry_context_create(const RetryConfig *config);
void retry_context_destroy(RetryContext *ctx);
void retry_context_reset(RetryContext *ctx);

// Configuration helpers
RetryConfig retry_config_default(void);
RetryConfig retry_config_aggressive(void);  // More retries, shorter delays
RetryConfig retry_config_conservative(void); // Fewer retries, longer delays

// Core retry function
RetryResult retry_execute(RetryContext *ctx, RetryOperation operation, void *user_data);

// Utility functions
bool is_retryable_http_status(int status);
bool is_retryable_error_code(int error_code);
int calculate_delay_ms(const RetryContext *ctx);
void add_jitter(int *delay_ms, int base_delay_ms);

// Rate limit specific functions
bool is_rate_limit_error(int http_status, const char *error_message);
int extract_retry_after_seconds(const char *response_headers);
int calculate_rate_limit_delay(int http_status, const char *response_headers, const RetryConfig *config);

// Debug and logging functions
void retry_log_attempt(const RetryContext *ctx, const char *operation_name);
void retry_log_failure(const RetryContext *ctx, const char *operation_name, const char *error);
void retry_log_success(const RetryContext *ctx, const char *operation_name);

// Constants
#define RETRY_DEFAULT_MAX_RETRIES 5
#define RETRY_DEFAULT_BASE_DELAY_MS 1000
#define RETRY_DEFAULT_MAX_DELAY_MS 30000
#define RETRY_DEFAULT_BACKOFF_MULTIPLIER 2.0
#define RETRY_JITTER_PERCENTAGE 0.1  // 10% jitter

#endif // RETRY_CONTEXT_H