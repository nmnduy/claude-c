/**
 * Unit tests for FormWork library
 *
 * Tests all core functionality including:
 * - JSON extraction (clean, markdown, extra text, arrays)
 * - Schema generation
 * - Prompt building
 * - Retry logic with mock LLM
 * - Metrics callbacks
 * - Error handling
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/formwork.h"

// Test counter
static int tests_passed = 0;
static int tests_failed = 0;

#define STRINGIFY(x) #x
#define TEST(name) \
    printf("\n🧪 Test: %s\n", STRINGIFY(name)); \
    if (test_##name()) { \
        printf("   ✓ PASS\n"); \
        tests_passed++; \
    } else { \
        printf("   ✗ FAIL\n"); \
        tests_failed++; \
    }

// ============================================================================
// Mock LLM implementations for testing
// ============================================================================

typedef struct {
    int call_count;
    int fail_until_attempt;  // Fail until this attempt number
} MockLLMState;

// Mock LLM that succeeds immediately
static char* mock_llm_success(const char *prompt, void *user_data) {
    (void)prompt;
    (void)user_data;
    return strdup("{\"result\": \"success\"}");
}

// Mock LLM that always fails
static char* mock_llm_failure(const char *prompt, void *user_data) {
    (void)prompt;
    (void)user_data;
    return strdup("This is not valid JSON");
}

// Mock LLM that fails N times before succeeding
static char* mock_llm_retry(const char *prompt, void *user_data) {
    MockLLMState *state = (MockLLMState*)user_data;
    state->call_count++;

    // Fail with bad JSON for the first few attempts
    if (state->call_count < state->fail_until_attempt) {
        return strdup("This is not valid JSON at all!");
    }

    // Success! Return valid JSON
    return strdup("```json\n"
                  "{\n"
                  "  \"name\": \"Alice Johnson\",\n"
                  "  \"age\": 30,\n"
                  "  \"email\": \"alice@example.com\",\n"
                  "  \"active\": true\n"
                  "}\n"
                  "```");
}

// ============================================================================
// Metrics tracking for tests
// ============================================================================

typedef struct {
    int start_count;
    int success_count;
    int retry_count;
    int failure_count;
} TestMetrics;

static void test_on_attempt_start(const char *target_name, int attempt, int max_retries, void *user_data) {
    (void)target_name;
    (void)attempt;
    (void)max_retries;
    TestMetrics *metrics = (TestMetrics*)user_data;
    metrics->start_count++;
}

static void test_on_attempt_success(const char *target_name, int attempt, int max_retries, void *user_data) {
    (void)target_name;
    (void)attempt;
    (void)max_retries;
    TestMetrics *metrics = (TestMetrics*)user_data;
    metrics->success_count++;
}

static void test_on_attempt_retry(const char *target_name, int attempt, int max_retries,
                                   const char *error_msg, void *user_data) {
    (void)target_name;
    (void)attempt;
    (void)max_retries;
    (void)error_msg;
    TestMetrics *metrics = (TestMetrics*)user_data;
    metrics->retry_count++;
}

static void test_on_final_failure(const char *target_name, int total_attempts,
                                   const char *error_msg, void *user_data) {
    (void)target_name;
    (void)total_attempts;
    (void)error_msg;
    TestMetrics *metrics = (TestMetrics*)user_data;
    metrics->failure_count++;
}

// ============================================================================
// Test: JSON extraction - clean JSON
// ============================================================================
static int test_json_extraction_clean(void) {
    const char *input = "{\"name\": \"test\", \"age\": 30}";
    cJSON *json = formwork_extract_json(input);

    if (!json) {
        printf("   Failed to extract JSON\n");
        return 0;
    }

    cJSON *name = cJSON_GetObjectItem(json, "name");
    cJSON *age = cJSON_GetObjectItem(json, "age");

    int success = (name && cJSON_IsString(name) && strcmp(name->valuestring, "test") == 0 &&
                   age && cJSON_IsNumber(age) && age->valueint == 30);

    cJSON_Delete(json);
    return success;
}

// ============================================================================
// Test: JSON extraction - with markdown
// ============================================================================
static int test_json_extraction_markdown(void) {
    const char *input = "```json\n{\"name\": \"test\"}\n```";
    cJSON *json = formwork_extract_json(input);

    if (!json) {
        printf("   Failed to extract JSON from markdown\n");
        return 0;
    }

    cJSON *name = cJSON_GetObjectItem(json, "name");
    int success = (name && cJSON_IsString(name) && strcmp(name->valuestring, "test") == 0);

    cJSON_Delete(json);
    return success;
}

// ============================================================================
// Test: JSON extraction - with extra text
// ============================================================================
static int test_json_extraction_extra_text(void) {
    const char *input = "Here is your data:\n{\"value\": 123}\nHope this helps!";
    cJSON *json = formwork_extract_json(input);

    if (!json) {
        printf("   Failed to extract JSON with extra text\n");
        return 0;
    }

    cJSON *value = cJSON_GetObjectItem(json, "value");
    int success = (value && cJSON_IsNumber(value) && value->valueint == 123);

    cJSON_Delete(json);
    return success;
}

// ============================================================================
// Test: JSON extraction - array
// ============================================================================
static int test_json_extraction_array(void) {
    const char *input = "[1, 2, 3]";
    cJSON *json = formwork_extract_json(input);

    if (!json) {
        printf("   Failed to extract JSON array\n");
        return 0;
    }

    int success = (cJSON_IsArray(json) && cJSON_GetArraySize(json) == 3);

    cJSON_Delete(json);
    return success;
}

// ============================================================================
// Test: JSON extraction - invalid input
// ============================================================================
static int test_json_extraction_invalid(void) {
    const char *input = "This is not JSON at all";
    cJSON *json = formwork_extract_json(input);

    // Should return NULL for invalid input
    return (json == NULL);
}

// ============================================================================
// Test: JSON extraction - empty input
// ============================================================================
static int test_json_extraction_empty(void) {
    cJSON *json = formwork_extract_json("");
    return (json == NULL);
}

// ============================================================================
// Test: Simple schema generation
// ============================================================================
static int test_schema_generation(void) {
    const char *fields[][2] = {
        {"name", "string"},
        {"age", "number"},
        {"active", "boolean"}
    };
    size_t field_count = 3;

    char *schema = formwork_build_simple_schema("TestType", fields, field_count);

    if (!schema) {
        printf("   Failed to generate schema\n");
        return 0;
    }

    // Verify schema contains expected elements
    int has_type = (strstr(schema, "\"type\"") != NULL);
    int has_properties = (strstr(schema, "\"properties\"") != NULL);
    int has_required = (strstr(schema, "\"required\"") != NULL);
    int has_name = (strstr(schema, "\"name\"") != NULL);
    int has_age = (strstr(schema, "\"age\"") != NULL);

    free(schema);

    return (has_type && has_properties && has_required && has_name && has_age);
}

// ============================================================================
// Test: Config initialization
// ============================================================================
static int test_config_init(void) {
    FormWorkConfig config;
    formwork_config_init(&config);

    return (config.max_retries == FORMWORK_DEFAULT_MAX_RETRIES &&
            config.retry_delay_ms == FORMWORK_DEFAULT_RETRY_DELAY_MS &&
            config.target_name == NULL &&
            config.base_prompt == NULL);
}

// ============================================================================
// Test: Prompt building
// ============================================================================
static int test_prompt_building(void) {
    FormWorkConfig config;
    formwork_config_init(&config);
    config.target_name = "TestType";
    config.base_prompt = "Generate a test object";
    config.json_schema = "{\"type\": \"object\"}";

    char *prompt = formwork_build_prompt(&config);

    if (!prompt) {
        printf("   Failed to build prompt\n");
        return 0;
    }

    // Verify prompt contains key elements
    int has_base = (strstr(prompt, "Generate a test object") != NULL);
    int has_format = (strstr(prompt, "Output format") != NULL);
    int has_schema = (strstr(prompt, "JSON Schema") != NULL);
    int has_type = (strstr(prompt, "TestType") != NULL);

    free(prompt);

    return (has_base && has_format && has_schema && has_type);
}

// ============================================================================
// Test: Retry prompt building
// ============================================================================
static int test_retry_prompt_building(void) {
    FormWorkConfig config;
    formwork_config_init(&config);
    config.target_name = "TestType";
    config.base_prompt = "Generate a test object";

    const char *error = "Invalid JSON format";
    const char *last_response = "{invalid json}";

    char *retry_prompt = formwork_build_retry_prompt(&config, error, last_response);

    if (!retry_prompt) {
        printf("   Failed to build retry prompt\n");
        return 0;
    }

    // Verify retry prompt contains key elements
    int has_original = (strstr(retry_prompt, "<original_request>") != NULL);
    int has_error = (strstr(retry_prompt, "<error>") != NULL);
    int has_previous = (strstr(retry_prompt, "<previous_response>") != NULL);
    int has_instructions = (strstr(retry_prompt, "<instructions>") != NULL);

    free(retry_prompt);

    return (has_original && has_error && has_previous && has_instructions);
}

// ============================================================================
// Test: Error string conversion
// ============================================================================
static int test_error_strings(void) {
    const char *success_str = formwork_error_string(FORMWORK_SUCCESS);
    const char *invalid_json_str = formwork_error_string(FORMWORK_ERROR_INVALID_JSON);
    const char *empty_str = formwork_error_string(FORMWORK_ERROR_EMPTY_RESPONSE);

    return (success_str != NULL && invalid_json_str != NULL && empty_str != NULL &&
            strstr(success_str, "Success") != NULL &&
            strstr(invalid_json_str, "Invalid JSON") != NULL &&
            strstr(empty_str, "Empty") != NULL);
}

// ============================================================================
// Test: Construct with success
// ============================================================================
static int test_construct_success(void) {
    FormWorkConfig config;
    formwork_config_init(&config);
    config.target_name = "TestType";
    config.base_prompt = "Generate a test object";
    config.llm_caller = mock_llm_success;
    config.max_retries = 3;
    config.retry_delay_ms = 10;  // Short delay for testing

    FormWorkResult result = formwork_construct(&config);

    int success = (result.error_code == FORMWORK_SUCCESS &&
                   result.json != NULL &&
                   result.attempts_used == 1);

    formwork_result_free(&result);
    return success;
}

// ============================================================================
// Test: Construct with max retries
// ============================================================================
static int test_construct_max_retries(void) {
    FormWorkConfig config;
    formwork_config_init(&config);
    config.target_name = "TestType";
    config.base_prompt = "Generate a test object";
    config.llm_caller = mock_llm_failure;
    config.max_retries = 3;
    config.retry_delay_ms = 10;  // Short delay for testing

    FormWorkResult result = formwork_construct(&config);

    int success = (result.error_code == FORMWORK_ERROR_INVALID_JSON &&
                   result.json == NULL &&
                   result.attempts_used == 3 &&
                   result.error_message != NULL);

    formwork_result_free(&result);
    return success;
}

// ============================================================================
// Test: Construct with invalid config
// ============================================================================
static int test_construct_invalid_config(void) {
    FormWorkConfig config;
    formwork_config_init(&config);
    // Don't set required fields

    FormWorkResult result = formwork_construct(&config);

    int success = (result.error_code == FORMWORK_ERROR_INVALID_CONFIG &&
                   result.json == NULL);

    formwork_result_free(&result);
    return success;
}

// ============================================================================
// Test: Retry behavior with metrics
// ============================================================================
static int test_retry_with_metrics(void) {
    MockLLMState llm_state = {0, 3};  // Fail first 2, succeed on 3rd
    TestMetrics metrics = {0, 0, 0, 0};

    FormWorkMetrics formwork_metrics = {
        .on_attempt_start = test_on_attempt_start,
        .on_attempt_success = test_on_attempt_success,
        .on_attempt_retry = test_on_attempt_retry,
        .on_final_failure = test_on_final_failure,
        .user_data = &metrics
    };

    FormWorkConfig config;
    formwork_config_init(&config);
    config.target_name = "TestType";
    config.base_prompt = "Generate test object";
    config.llm_caller = mock_llm_retry;
    config.llm_user_data = &llm_state;
    config.max_retries = 5;
    config.retry_delay_ms = 10;  // Short delay for testing
    config.metrics = &formwork_metrics;

    FormWorkResult result = formwork_construct(&config);

    int success = (result.error_code == FORMWORK_SUCCESS &&
                   result.json != NULL &&
                   result.attempts_used == 3 &&
                   metrics.start_count == 3 &&
                   metrics.success_count == 1 &&
                   metrics.retry_count == 2 &&
                   metrics.failure_count == 0);

    formwork_result_free(&result);
    return success;
}

// ============================================================================
// Test: Metrics on final failure
// ============================================================================
static int test_metrics_final_failure(void) {
    TestMetrics metrics = {0, 0, 0, 0};

    FormWorkMetrics formwork_metrics = {
        .on_attempt_start = test_on_attempt_start,
        .on_attempt_success = test_on_attempt_success,
        .on_attempt_retry = test_on_attempt_retry,
        .on_final_failure = test_on_final_failure,
        .user_data = &metrics
    };

    FormWorkConfig config;
    formwork_config_init(&config);
    config.target_name = "TestType";
    config.base_prompt = "Generate test object";
    config.llm_caller = mock_llm_failure;
    config.max_retries = 3;
    config.retry_delay_ms = 10;
    config.metrics = &formwork_metrics;

    FormWorkResult result = formwork_construct(&config);

    int success = (result.error_code == FORMWORK_ERROR_INVALID_JSON &&
                   metrics.start_count == 3 &&
                   metrics.success_count == 0 &&
                   metrics.retry_count == 2 &&  // 2 retries (between attempts 1-2 and 2-3)
                   metrics.failure_count == 1);

    formwork_result_free(&result);
    return success;
}

// ============================================================================
// Test: Complex JSON with nested objects
// ============================================================================
static int test_json_nested_objects(void) {
    const char *input = "{\"user\": {\"name\": \"Alice\", \"age\": 30}, \"active\": true}";
    cJSON *json = formwork_extract_json(input);

    if (!json) {
        printf("   Failed to extract nested JSON\n");
        return 0;
    }

    cJSON *user = cJSON_GetObjectItem(json, "user");
    cJSON *name = cJSON_GetObjectItem(user, "name");
    cJSON *active = cJSON_GetObjectItem(json, "active");

    int success = (user && cJSON_IsObject(user) &&
                   name && cJSON_IsString(name) && strcmp(name->valuestring, "Alice") == 0 &&
                   active && cJSON_IsBool(active));

    cJSON_Delete(json);
    return success;
}

// ============================================================================
// Test: Retry prompt contains error context
// ============================================================================
static int test_retry_prompt_context(void) {
    FormWorkConfig config;
    formwork_config_init(&config);
    config.target_name = "TestType";
    config.base_prompt = "Generate a test object with field X";

    const char *error = "Missing required field 'X'";
    const char *last_response = "{\"Y\": \"wrong\"}";

    char *retry_prompt = formwork_build_retry_prompt(&config, error, last_response);

    if (!retry_prompt) {
        printf("   Failed to build retry prompt\n");
        return 0;
    }

    // Verify all context is present
    int has_original = (strstr(retry_prompt, "Generate a test object with field X") != NULL);
    int has_error_text = (strstr(retry_prompt, "Missing required field 'X'") != NULL);
    int has_response = (strstr(retry_prompt, "{\"Y\": \"wrong\"}") != NULL);
    int has_critical = (strstr(retry_prompt, "CRITICAL") != NULL);

    free(retry_prompt);

    return (has_original && has_error_text && has_response && has_critical);
}

// ============================================================================
// Main test runner
// ============================================================================
int main(void) {
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════╗\n");
    printf("║         FormWork Library - Unit Test Suite               ║\n");
    printf("╚═══════════════════════════════════════════════════════════╝\n");

    TEST(json_extraction_clean);
    TEST(json_extraction_markdown);
    TEST(json_extraction_extra_text);
    TEST(json_extraction_array);
    TEST(json_extraction_invalid);
    TEST(json_extraction_empty);
    TEST(json_nested_objects);
    TEST(schema_generation);
    TEST(config_init);
    TEST(prompt_building);
    TEST(retry_prompt_building);
    TEST(retry_prompt_context);
    TEST(error_strings);
    TEST(construct_success);
    TEST(construct_max_retries);
    TEST(construct_invalid_config);
    TEST(retry_with_metrics);
    TEST(metrics_final_failure);

    printf("\n");
    printf("═══════════════════════════════════════════════════════════\n");
    printf("Test Results:\n");
    printf("  ✓ Passed: %d\n", tests_passed);
    if (tests_failed > 0) {
        printf("  ✗ Failed: %d\n", tests_failed);
    }
    printf("  Total:  %d\n", tests_passed + tests_failed);
    printf("═══════════════════════════════════════════════════════════\n");
    printf("\n");

    return (tests_failed == 0) ? 0 : 1;
}
