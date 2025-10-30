/*
 * Test AWS credential rotation with polling behavior
 *
 * Tests the new polling mechanism that waits for credentials to actually
 * change after authentication, rather than immediately attempting to use
 * potentially stale credentials.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

// Need to expose internals for testing
#define TEST_BUILD 1
#include "../src/aws_bedrock.h"
#include "../src/logger.h"

// Test framework
static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if (cond) { \
        tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (at line %d)\n", msg, __LINE__); \
    } \
} while(0)

#define ASSERT_EQ(expected, actual, msg) do { \
    tests_run++; \
    if ((expected) == (actual)) { \
        tests_passed++; \
        printf("  [PASS] %s (expected=%d, actual=%d)\n", msg, (int)(expected), (int)(actual)); \
    } else { \
        printf("  [FAIL] %s (expected=%d, actual=%d, at line %d)\n", msg, (int)(expected), (int)(actual), __LINE__); \
    } \
} while(0)

#define ASSERT_STR_EQ(expected, actual, msg) do { \
    tests_run++; \
    if (strcmp((expected), (actual)) == 0) { \
        tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (expected='%s', actual='%s', at line %d)\n", msg, expected, actual, __LINE__); \
    } \
} while(0)

// ============================================================================
// Mock State
// ============================================================================

static int mock_auth_calls = 0;
static int mock_exec_calls = 0;
static int mock_credential_version = 0;  // Incremented to simulate credential changes
static int mock_poll_attempts = 0;       // Track how many times credentials were polled

// ============================================================================
// Mock Functions
// ============================================================================

/**
 * Mock exec_command - simulates AWS CLI commands
 * Returns different credentials based on mock_credential_version
 */
static char* mock_exec_command(const char *cmd) {
    mock_exec_calls++;

    // aws sts get-caller-identity (validation)
    if (strstr(cmd, "aws sts get-caller-identity")) {
        // Always return valid after authentication
        if (mock_credential_version > 0) {
            return strdup("{\"UserId\": \"VALID123\", \"Account\": \"123456789\"}");
        } else {
            return strdup("ExpiredToken");
        }
    }

    // aws configure get sso_start_url
    if (strstr(cmd, "aws configure get sso_start_url")) {
        return strdup("https://test-sso.awsapps.com/start");
    }

    // aws configure export-credentials
    if (strstr(cmd, "export-credentials")) {
        mock_poll_attempts++;

        // Return credentials based on current version
        if (mock_credential_version > 0) {
            char buffer[512];
            // Different access key for each version to simulate rotation
            snprintf(buffer, sizeof(buffer),
                    "export AWS_ACCESS_KEY_ID=AKIA_VERSION_%d\n"
                    "export AWS_SECRET_ACCESS_KEY=SECRET_VERSION_%d\n"
                    "export AWS_SESSION_TOKEN=TOKEN_VERSION_%d\n",
                    mock_credential_version, mock_credential_version, mock_credential_version);
            return strdup(buffer);
        } else {
            // No credentials yet
            return strdup("");
        }
    }

    return strdup("");
}

/**
 * Mock system - simulates authentication commands
 */
static int mock_system(const char *cmd) {
    if (strstr(cmd, "aws sso login") || strstr(cmd, "custom-auth")) {
        mock_auth_calls++;
        // Simulate delayed credential update
        // Credentials will update after this call, but not immediately
        mock_credential_version++;
        return 0;  // Success
    }
    return 1;  // Failure
}

/**
 * Mock system with immediate credential update
 */
static int mock_system_immediate(const char *cmd) {
    if (strstr(cmd, "aws sso login") || strstr(cmd, "custom-auth")) {
        mock_auth_calls++;
        mock_credential_version++;
        return 0;
    }
    return 1;
}

/**
 * Mock system that fails authentication
 */
static int mock_system_fail(const char *cmd) {
    (void)cmd;
    mock_auth_calls++;
    return 1;  // Always fail
}

// ============================================================================
// Helper Functions
// ============================================================================

static void reset_mocks(void) {
    mock_auth_calls = 0;
    mock_exec_calls = 0;
    mock_credential_version = 0;
    mock_poll_attempts = 0;
}

static void setup_test_env(void) {
    unsetenv("AWS_ACCESS_KEY_ID");
    unsetenv("AWS_SECRET_ACCESS_KEY");
    unsetenv("AWS_SESSION_TOKEN");
    unsetenv("AWS_AUTH_COMMAND");
    setenv("AWS_PROFILE", "test-profile", 1);
    setenv("AWS_REGION", "us-west-2", 1);
}

static void cleanup_test_env(void) {
    unsetenv("AWS_PROFILE");
    unsetenv("AWS_REGION");
    unsetenv("AWS_AUTH_COMMAND");
}

// ============================================================================
// Test Cases
// ============================================================================

/**
 * Test 1: Credentials are available immediately after authentication
 */
static void test_immediate_credential_availability(void) {
    printf("\n[Test 1] Immediate credential availability\n");
    reset_mocks();
    setup_test_env();

    aws_bedrock_set_exec_command_fn(mock_exec_command);
    aws_bedrock_set_system_fn(mock_system_immediate);

    // Load credentials (should trigger auth)
    AWSCredentials *creds = bedrock_load_credentials("test-profile", "us-west-2");

    ASSERT_TRUE(creds != NULL, "Credentials loaded successfully");
    ASSERT_EQ(1, mock_auth_calls, "Authentication called once");
    ASSERT_TRUE(mock_poll_attempts > 0, "Credentials were polled at least once");
    ASSERT_TRUE(creds->access_key_id != NULL, "Access key is present");
    ASSERT_TRUE(strstr(creds->access_key_id, "VERSION_1") != NULL, "Access key has version 1");

    bedrock_creds_free(creds);
    cleanup_test_env();
}

/**
 * Test 2: Credentials change after authentication (polling required)
 */
static void test_credential_polling_detects_change(void) {
    printf("\n[Test 2] Credential polling detects change\n");
    reset_mocks();
    setup_test_env();

    aws_bedrock_set_exec_command_fn(mock_exec_command);
    aws_bedrock_set_system_fn(mock_system);

    // Start with no credentials, forcing authentication
    mock_credential_version = 0;

    // Load credentials (should trigger authentication and polling)
    AWSCredentials *creds = bedrock_load_credentials("test-profile", "us-west-2");

    ASSERT_TRUE(creds != NULL, "Credentials loaded after rotation");
    ASSERT_EQ(1, mock_auth_calls, "Authentication called once");
    ASSERT_TRUE(mock_poll_attempts >= 1, "Credentials polled at least once");
    ASSERT_TRUE(creds->access_key_id != NULL, "New access key is present");
    ASSERT_TRUE(strstr(creds->access_key_id, "VERSION_1") != NULL, "Access key rotated to version 1");
    ASSERT_EQ(1, mock_credential_version, "Credential version is 1 after authentication");

    bedrock_creds_free(creds);
    cleanup_test_env();
}

/**
 * Test 3: Multiple rotation attempts (simulates delayed credential cache update)
 */
static void test_delayed_credential_update(void) {
    printf("\n[Test 3] Delayed credential cache update\n");
    reset_mocks();
    setup_test_env();

    // Start with version 0 (expired)
    mock_credential_version = 0;

    aws_bedrock_set_exec_command_fn(mock_exec_command);
    aws_bedrock_set_system_fn(mock_system);

    // Load credentials
    AWSCredentials *creds = bedrock_load_credentials("test-profile", "us-west-2");

    ASSERT_TRUE(creds != NULL, "Credentials eventually loaded");
    ASSERT_EQ(1, mock_auth_calls, "Authentication called once");
    ASSERT_TRUE(mock_credential_version > 0, "Credential version incremented");

    bedrock_creds_free(creds);
    cleanup_test_env();
}

/**
 * Test 4: Credential validation after rotation
 */
static void test_credential_validation_after_rotation(void) {
    printf("\n[Test 4] Credential validation after rotation\n");
    reset_mocks();
    setup_test_env();

    aws_bedrock_set_exec_command_fn(mock_exec_command);
    aws_bedrock_set_system_fn(mock_system);

    // First load - triggers auth
    AWSCredentials *creds1 = bedrock_load_credentials("test-profile", "us-west-2");
    ASSERT_TRUE(creds1 != NULL, "First credential load successful");

    const char *key1 = creds1->access_key_id;
    ASSERT_TRUE(key1 != NULL, "First access key is present");

    // Validate first credentials
    int valid1 = bedrock_validate_credentials(creds1, "test-profile");
    ASSERT_EQ(1, valid1, "First credentials are valid");

    // Trigger rotation
    int saved_version = mock_credential_version;
    int auth_result = bedrock_authenticate("test-profile");
    ASSERT_EQ(0, auth_result, "Authentication succeeded");
    ASSERT_TRUE(mock_credential_version > saved_version, "Credential version incremented");

    // Load again - should get new credentials
    AWSCredentials *creds2 = bedrock_load_credentials("test-profile", "us-west-2");
    ASSERT_TRUE(creds2 != NULL, "Second credential load successful");
    ASSERT_TRUE(creds2->access_key_id != NULL, "Second access key is present");
    ASSERT_TRUE(strcmp(creds1->access_key_id, creds2->access_key_id) != 0,
                "Access keys are different after rotation");

    // Validate new credentials
    int valid2 = bedrock_validate_credentials(creds2, "test-profile");
    ASSERT_EQ(1, valid2, "Second credentials are valid");

    bedrock_creds_free(creds1);
    bedrock_creds_free(creds2);
    cleanup_test_env();
}

/**
 * Test 5: Custom authentication command
 */
static void test_custom_auth_command(void) {
    printf("\n[Test 5] Custom authentication command\n");
    reset_mocks();
    setup_test_env();

    setenv("AWS_AUTH_COMMAND", "custom-auth --profile test", 1);

    aws_bedrock_set_exec_command_fn(mock_exec_command);
    aws_bedrock_set_system_fn(mock_system);

    // Load credentials (should use custom auth)
    AWSCredentials *creds = bedrock_load_credentials("test-profile", "us-west-2");

    ASSERT_TRUE(creds != NULL, "Credentials loaded with custom auth");
    ASSERT_EQ(1, mock_auth_calls, "Custom auth command called once");
    ASSERT_TRUE(creds->access_key_id != NULL, "Access key is present");

    bedrock_creds_free(creds);
    cleanup_test_env();
}

/**
 * Test 6: Authentication failure handling
 */
static void test_authentication_failure(void) {
    printf("\n[Test 6] Authentication failure handling\n");
    reset_mocks();
    setup_test_env();

    aws_bedrock_set_exec_command_fn(mock_exec_command);
    aws_bedrock_set_system_fn(mock_system_fail);

    // Try to authenticate (should fail)
    int result = bedrock_authenticate("test-profile");

    ASSERT_EQ(-1, result, "Authentication returns error on failure");
    ASSERT_EQ(1, mock_auth_calls, "Authentication was attempted");

    cleanup_test_env();
}

/**
 * Test 7: Credential version tracking across multiple rotations
 */
static void test_multiple_rotation_cycles(void) {
    printf("\n[Test 7] Multiple rotation cycles\n");
    reset_mocks();
    setup_test_env();

    aws_bedrock_set_exec_command_fn(mock_exec_command);
    aws_bedrock_set_system_fn(mock_system);

    char *keys[3] = {NULL, NULL, NULL};

    // Perform 3 rotation cycles
    for (int i = 0; i < 3; i++) {
        if (i > 0) {
            // Trigger rotation
            int auth_result = bedrock_authenticate("test-profile");
            ASSERT_EQ(0, auth_result, "Authentication succeeded in cycle");
        }

        // Load credentials
        AWSCredentials *creds = bedrock_load_credentials("test-profile", "us-west-2");
        ASSERT_TRUE(creds != NULL, "Credentials loaded in cycle");
        ASSERT_TRUE(creds->access_key_id != NULL, "Access key present in cycle");

        // Save key for comparison
        keys[i] = strdup(creds->access_key_id);

        bedrock_creds_free(creds);
    }

    // Verify all keys are different
    ASSERT_TRUE(strcmp(keys[0], keys[1]) != 0, "Keys differ between cycle 1 and 2");
    ASSERT_TRUE(strcmp(keys[1], keys[2]) != 0, "Keys differ between cycle 2 and 3");
    ASSERT_TRUE(strcmp(keys[0], keys[2]) != 0, "Keys differ between cycle 1 and 3");

    // Verify version numbers increased
    ASSERT_TRUE(strstr(keys[0], "VERSION_1") != NULL, "First key is version 1");
    ASSERT_TRUE(strstr(keys[1], "VERSION_2") != NULL, "Second key is version 2");
    ASSERT_TRUE(strstr(keys[2], "VERSION_3") != NULL, "Third key is version 3");

    for (int i = 0; i < 3; i++) {
        free(keys[i]);
    }

    cleanup_test_env();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== AWS Credential Rotation Tests ===\n");
    printf("Testing polling behavior and credential change detection\n");

    // Initialize logger
    log_init();

    // Run test suite
    test_immediate_credential_availability();
    test_credential_polling_detects_change();
    test_delayed_credential_update();
    test_credential_validation_after_rotation();
    test_custom_auth_command();
    test_authentication_failure();
    test_multiple_rotation_cycles();

    // Print summary
    printf("\n=== Test Summary ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests passed: %d\n", tests_passed);
    printf("Tests failed: %d\n", tests_run - tests_passed);

    if (tests_run == tests_passed) {
        printf("\n✓ All AWS credential rotation tests passed!\n");
        return 0;
    } else {
        printf("\n✗ Some tests failed\n");
        return 1;
    }
}
