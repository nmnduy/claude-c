#ifdef TEST_BUILD
#undef TEST_BUILD
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aws_bedrock.h"
#include "logger.h"

// Simple test framework
static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; printf("[PASS] %s\n", msg); } \
    else { printf("[FAIL] %s\n", msg); } \
} while(0)

// Mock state
static int auth_done = 0;
static int system_calls = 0;
static int exec_calls = 0;

// Mock exec_command: simulate credential sources and validation
static char* exec_command_mock(const char *cmd) {
    exec_calls++;
    // Validation via aws sts get-caller-identity
    if (strstr(cmd, "aws sts get-caller-identity")) {
        if (auth_done) {
            return strdup("{\"UserId\": \"ABC123\"}");
        } else {
            return strdup("ExpiredToken");
        }
    }
    // SSO start URL detection
    if (strstr(cmd, "aws configure get sso_start_url")) {
        return strdup("https://dummy-sso-url");
    }
    // export-credentials output
    if (strstr(cmd, "export-credentials")) {
        if (auth_done) {
            return strdup("export AWS_ACCESS_KEY_ID=AKIA\nexport AWS_SECRET_ACCESS_KEY=SECRET\n");
        } else {
            return strdup("");
        }
    }
    // Fallback empty
    return strdup("");
}

// Mock system: capture auth commands (SSO and custom)
static int system_mock(const char *cmd) {
    system_calls++;
    if (strstr(cmd, "aws sso login")) {
        auth_done = 1;
        return 0;
    }
    if (strstr(cmd, "custom-auth")) {
        auth_done = 1;
        return 0;
    }
    return 1;
}

static void test_sso_reauthentication(void) {
    printf("\nTest: SSO re-authentication flow\n");
    unsetenv("AWS_ACCESS_KEY_ID");
    unsetenv("AWS_SECRET_ACCESS_KEY");
    unsetenv("AWS_SESSION_TOKEN");
    unsetenv("AWS_AUTH_COMMAND");
    unsetenv("AWS_PROFILE");

    aws_bedrock_set_exec_command_fn(exec_command_mock);
    aws_bedrock_set_system_fn(system_mock);
    auth_done = exec_calls = system_calls = 0;

    AWSCredentials *creds = bedrock_load_credentials(NULL, NULL);
    ASSERT_TRUE(creds != NULL, "Credentials returned after SSO auth");
    ASSERT_TRUE(system_calls == 1, "One system call to aws sso login");
    ASSERT_TRUE(exec_calls >= 3, "exec_command called for validation and exports");

    bedrock_creds_free(creds);
}

static void test_custom_auth_command(void) {
    printf("\nTest: AWS_AUTH_COMMAND fallback without valid creds\n");
    unsetenv("AWS_ACCESS_KEY_ID");
    unsetenv("AWS_SECRET_ACCESS_KEY");
    unsetenv("AWS_SESSION_TOKEN");
    setenv("AWS_AUTH_COMMAND", "echo custom-auth && return 0", 1);
    unsetenv("AWS_PROFILE");

    aws_bedrock_set_exec_command_fn(exec_command_mock);
    aws_bedrock_set_system_fn(system_mock);
    auth_done = exec_calls = system_calls = 0;

    AWSCredentials *creds = bedrock_load_credentials(NULL, NULL);
    ASSERT_TRUE(system_calls >= 1, "Custom auth command invoked");
    ASSERT_TRUE(creds == NULL, "Credential load fails without valid exports");

    unsetenv("AWS_AUTH_COMMAND");
}

static void test_env_credentials_expired_sso_reauth(void) {
    printf("\nTest: Env credentials expired then SSO re-authentication\n");
    setenv("AWS_ACCESS_KEY_ID", "EXPIRE", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "EXPIRE_SECRET", 1);
    unsetenv("AWS_SESSION_TOKEN");
    unsetenv("AWS_AUTH_COMMAND");
    unsetenv("AWS_PROFILE");

    aws_bedrock_set_exec_command_fn(exec_command_mock);
    aws_bedrock_set_system_fn(system_mock);
    auth_done = exec_calls = system_calls = 0;

    AWSCredentials *creds = bedrock_load_credentials(NULL, NULL);
    ASSERT_TRUE(creds != NULL, "Credentials returned after env expired and SSO reauth");
    ASSERT_TRUE(system_calls == 1, "One system call to aws sso login");
    ASSERT_TRUE(exec_calls >= 2, "exec_command called for validation and exports");

    bedrock_creds_free(creds);
}

static void test_env_credentials_expired_custom_auth(void) {
    printf("\nTest: Env credentials expired then custom auth command\n");
    setenv("AWS_ACCESS_KEY_ID", "EXPIRE", 1);
    setenv("AWS_SECRET_ACCESS_KEY", "EXPIRE_SECRET", 1);
    unsetenv("AWS_SESSION_TOKEN");
    setenv("AWS_AUTH_COMMAND", "echo custom-auth && return 0", 1);
    unsetenv("AWS_PROFILE");

    aws_bedrock_set_exec_command_fn(exec_command_mock);
    aws_bedrock_set_system_fn(system_mock);
    auth_done = exec_calls = system_calls = 0;

    AWSCredentials *creds = bedrock_load_credentials(NULL, NULL);
    ASSERT_TRUE(creds != NULL, "Credentials returned after env expired and custom auth");
    ASSERT_TRUE(system_calls >= 1, "Custom auth command invoked");
    ASSERT_TRUE(exec_calls >= 2, "exec_command called for validation and exports");

    bedrock_creds_free(creds);
    unsetenv("AWS_AUTH_COMMAND");
}

int main(void) {
    test_sso_reauthentication();
    test_custom_auth_command();
    test_env_credentials_expired_sso_reauth();
    test_env_credentials_expired_custom_auth();

    printf("\nTests run: %d, passed: %d, failed: %d\n",
           tests_run, tests_passed, tests_run - tests_passed);
    return (tests_run == tests_passed) ? 0 : 1;
}
