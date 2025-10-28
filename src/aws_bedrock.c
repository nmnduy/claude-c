/*
 * aws_bedrock.c - AWS Bedrock provider implementation
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "aws_bedrock.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Hex encode a buffer
 */
static char* hex_encode(const unsigned char *data, size_t len) {
    char *hex = malloc(len * 2 + 1);
    if (!hex) return NULL;

    for (size_t i = 0; i < len; i++) {
        snprintf(hex + (i * 2), 3, "%02x", data[i]);
    }
    hex[len * 2] = '\0';
    return hex;
}

/**
 * URL encode a string (for AWS SigV4)
 */
static char* url_encode(const char *str, int encode_slash) {
    if (!str) return NULL;

    size_t len = strlen(str);
    char *encoded = malloc(len * 3 + 1);  // Worst case: every char becomes %XX
    if (!encoded) return NULL;

    char *out = encoded;
    for (const char *p = str; *p; p++) {
        if (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            *out++ = *p;
        } else if (*p == '/' && !encode_slash) {
            *out++ = *p;
        } else {
            snprintf(out, 4, "%%%02X", (unsigned char)*p);
            out += 3;
        }
    }
    *out = '\0';
    return encoded;
}

/**
 * Get current timestamp in ISO8601 format (YYYYMMDDTHHMMSSZ)
 */
static char* get_iso8601_timestamp(void) {
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char *timestamp = malloc(17);  // YYYYMMDDTHHMMSSZ + null
    if (!timestamp) return NULL;

    strftime(timestamp, 17, "%Y%m%dT%H%M%SZ", tm);
    return timestamp;
}

/**
 * Get current date in YYYYMMDD format
 */
static char* get_date_stamp(void) {
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char *date = malloc(9);  // YYYYMMDD + null
    if (!date) return NULL;

    strftime(date, 9, "%Y%m%d", tm);
    return date;
}

/**
 * HMAC-SHA256
 */
static unsigned char* hmac_sha256(const unsigned char *key, size_t key_len,
                                   const unsigned char *data, size_t data_len,
                                   unsigned char *output) {
    unsigned int len = 32;
    return HMAC(EVP_sha256(), key, (int)key_len, data, data_len, output, &len);
}

/**
 * SHA256 hash
 */
static char* sha256_hash(const char *data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)data, strlen(data), hash);
    return hex_encode(hash, SHA256_DIGEST_LENGTH);
}

/**
 * Execute a command and return its output
 */
static char* exec_command(const char *command) {
    FILE *fp = popen(command, "r");
    if (!fp) {
        LOG_ERROR("Failed to execute command: %s", command);
        return NULL;
    }

    char *output = NULL;
    size_t size = 0;
    size_t capacity = 1024;
    output = malloc(capacity);
    if (!output) {
        pclose(fp);
        return NULL;
    }

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), fp)) {
        size_t len = strlen(buffer);
        if (size + len + 1 > capacity) {
            capacity *= 2;
            char *new_output = realloc(output, capacity);
            if (!new_output) {
                free(output);
                pclose(fp);
                return NULL;
            }
            output = new_output;
        }
        strcpy(output + size, buffer);
        size += len;
    }

    pclose(fp);

    // Remove trailing newline
    if (size > 0 && output[size - 1] == '\n') {
        output[size - 1] = '\0';
    }

    return output;
}

// ============================================================================
// Public API Implementation
// ============================================================================

int bedrock_is_enabled(void) {
    const char *enabled = getenv(ENV_USE_BEDROCK);
    return enabled && (strcmp(enabled, "true") == 0 || strcmp(enabled, "1") == 0);
}

BedrockConfig* bedrock_config_init(const char *model_id) {
    if (!bedrock_is_enabled()) {
        return NULL;
    }

    BedrockConfig *config = calloc(1, sizeof(BedrockConfig));
    if (!config) {
        LOG_ERROR("Failed to allocate BedrockConfig");
        return NULL;
    }

    config->enabled = 1;

    // Get region from environment
    const char *region = getenv(ENV_AWS_REGION);
    if (!region) {
        region = "us-west-2";  // Default region
        LOG_WARN("AWS_REGION not set, using default: %s", region);
    }
    config->region = strdup(region);

    // Get model ID
    if (model_id) {
        config->model_id = strdup(model_id);
    } else {
        LOG_ERROR("Model ID is required for Bedrock");
        bedrock_config_free(config);
        return NULL;
    }

    // Build endpoint URL
    config->endpoint = bedrock_build_endpoint(region, model_id);
    if (!config->endpoint) {
        LOG_ERROR("Failed to build Bedrock endpoint");
        bedrock_config_free(config);
        return NULL;
    }

    // Load credentials
    const char *profile = getenv(ENV_AWS_PROFILE);
    config->creds = bedrock_load_credentials(profile, region);
    if (!config->creds) {
        LOG_ERROR("Failed to load AWS credentials");
        bedrock_config_free(config);
        return NULL;
    }

    LOG_INFO("Bedrock config initialized: region=%s, model=%s", region, model_id);
    return config;
}

void bedrock_config_free(BedrockConfig *config) {
    if (!config) return;

    free(config->region);
    free(config->model_id);
    free(config->endpoint);
    bedrock_creds_free(config->creds);
    free(config);
}

AWSCredentials* bedrock_load_credentials(const char *profile, const char *region) {
    AWSCredentials *creds = calloc(1, sizeof(AWSCredentials));
    if (!creds) {
        LOG_ERROR("Failed to allocate AWSCredentials");
        return NULL;
    }

    // Try environment variables first
    const char *access_key = getenv(ENV_AWS_ACCESS_KEY_ID);
    const char *secret_key = getenv(ENV_AWS_SECRET_ACCESS_KEY);
    const char *session_token = getenv(ENV_AWS_SESSION_TOKEN);

    if (access_key && secret_key) {
        creds->access_key_id = strdup(access_key);
        creds->secret_access_key = strdup(secret_key);
        if (session_token) {
            creds->session_token = strdup(session_token);
        }
        creds->region = strdup(region ? region : "us-west-2");
        LOG_INFO("Loaded AWS credentials from environment variables");
        return creds;
    }

    // Try AWS CLI to get credentials (use export-credentials for temp creds support)
    char command[512];
    const char *profile_arg = profile ? profile : "default";

    // First try export-credentials which handles temporary credentials properly
    snprintf(command, sizeof(command),
             "aws configure export-credentials --profile %s --format env 2>/dev/null",
             profile_arg);
    char *export_output = exec_command(command);

    if (export_output && strlen(export_output) > 0) {
        // Parse the export output (format: export AWS_ACCESS_KEY_ID=xxx\nexport AWS_SECRET_ACCESS_KEY=yyy\n...)
        char *line = strtok(export_output, "\n");
        while (line) {
            // Skip "export " prefix if present
            const char *value_start = line;
            if (strncmp(line, "export ", 7) == 0) {
                value_start = line + 7;
            }

            if (strncmp(value_start, "AWS_ACCESS_KEY_ID=", 18) == 0) {
                creds->access_key_id = strdup(value_start + 18);
            } else if (strncmp(value_start, "AWS_SECRET_ACCESS_KEY=", 22) == 0) {
                creds->secret_access_key = strdup(value_start + 22);
            } else if (strncmp(value_start, "AWS_SESSION_TOKEN=", 18) == 0) {
                creds->session_token = strdup(value_start + 18);
            }
            line = strtok(NULL, "\n");
        }
        free(export_output);

        if (creds->access_key_id && creds->secret_access_key) {
            creds->region = strdup(region ? region : "us-west-2");
            creds->profile = strdup(profile_arg);
            LOG_INFO("Loaded AWS credentials from AWS CLI (profile: %s, with_session_token: %s)",
                     profile_arg, creds->session_token ? "yes" : "no");
            return creds;
        }
    } else {
        free(export_output);
    }

    // Fallback: Try aws configure get for static credentials
    snprintf(command, sizeof(command),
             "aws configure get aws_access_key_id --profile %s 2>/dev/null",
             profile_arg);
    char *key_id = exec_command(command);

    snprintf(command, sizeof(command),
             "aws configure get aws_secret_access_key --profile %s 2>/dev/null",
             profile_arg);
    char *secret = exec_command(command);

    if (key_id && secret && strlen(key_id) > 0 && strlen(secret) > 0) {
        creds->access_key_id = key_id;
        creds->secret_access_key = secret;
        creds->region = strdup(region ? region : "us-west-2");
        creds->profile = strdup(profile_arg);
        LOG_INFO("Loaded AWS credentials from AWS CLI config (profile: %s)", profile_arg);
        return creds;
    }

    free(key_id);
    free(secret);

    // Try AWS SSO
    LOG_INFO("Attempting to load credentials via AWS SSO for profile: %s", profile_arg);

    // Check if profile uses SSO
    snprintf(command, sizeof(command),
             "aws configure get sso_start_url --profile %s 2>/dev/null",
             profile_arg);
    char *sso_url = exec_command(command);

    if (sso_url && strlen(sso_url) > 0) {
        LOG_INFO("Profile %s uses AWS SSO, attempting to get cached credentials", profile_arg);
        free(sso_url);

        // Try to export credentials using AWS CLI
        snprintf(command, sizeof(command),
                 "aws configure export-credentials --profile %s --format env 2>/dev/null",
                 profile_arg);
        char *sso_export_output = exec_command(command);

        if (sso_export_output && strlen(sso_export_output) > 0) {
            // Parse the export output (format: AWS_ACCESS_KEY_ID=xxx\nAWS_SECRET_ACCESS_KEY=yyy\n...)
            char *line = strtok(sso_export_output, "\n");
            while (line) {
                if (strncmp(line, "AWS_ACCESS_KEY_ID=", 18) == 0) {
                    creds->access_key_id = strdup(line + 18);
                } else if (strncmp(line, "AWS_SECRET_ACCESS_KEY=", 22) == 0) {
                    creds->secret_access_key = strdup(line + 22);
                } else if (strncmp(line, "AWS_SESSION_TOKEN=", 18) == 0) {
                    creds->session_token = strdup(line + 18);
                }
                line = strtok(NULL, "\n");
            }
            free(sso_export_output);

            if (creds->access_key_id && creds->secret_access_key) {
                creds->region = strdup(region ? region : "us-west-2");
                creds->profile = strdup(profile_arg);
                LOG_INFO("Loaded AWS credentials from SSO cache");
                return creds;
            }
        } else {
            free(sso_export_output);
        }

        // SSO credentials not found, need to authenticate
        LOG_WARN("AWS SSO credentials not found or expired for profile: %s", profile_arg);
        if (bedrock_authenticate(profile_arg) == 0) {
            // Retry loading credentials after authentication
            return bedrock_load_credentials(profile, region);
        }
    } else {
        free(sso_url);
    }

    LOG_ERROR("Failed to load AWS credentials from any source");
    bedrock_creds_free(creds);
    return NULL;
}

void bedrock_creds_free(AWSCredentials *creds) {
    if (!creds) return;

    free(creds->access_key_id);
    free(creds->secret_access_key);
    free(creds->session_token);
    free(creds->region);
    free(creds->profile);
    free(creds);
}

int bedrock_validate_credentials(AWSCredentials *creds, const char *profile) {
    if (!creds || !creds->access_key_id || !creds->secret_access_key) {
        return 0;
    }

    (void)profile;  // Unused parameter

    // Set environment variables for the validation call
    char env_cmd[1024];
    snprintf(env_cmd, sizeof(env_cmd),
             "AWS_ACCESS_KEY_ID='%s' AWS_SECRET_ACCESS_KEY='%s' %s aws sts get-caller-identity --region %s 2>&1",
             creds->access_key_id,
             creds->secret_access_key,
             creds->session_token ? "AWS_SESSION_TOKEN='" : "",
             creds->region ? creds->region : "us-west-2");

    if (creds->session_token) {
        strncat(env_cmd, creds->session_token, sizeof(env_cmd) - strlen(env_cmd) - 1);
        strncat(env_cmd, "' ", sizeof(env_cmd) - strlen(env_cmd) - 1);
    }

    char *output = exec_command(env_cmd);
    int valid = 0;

    if (output) {
        // Check if output contains error messages
        if (strstr(output, "ExpiredToken") || strstr(output, "InvalidToken") ||
            strstr(output, "AccessDenied")) {
            LOG_WARN("AWS credentials are invalid or expired");
            valid = 0;
        } else if (strstr(output, "UserId") || strstr(output, "Account")) {
            LOG_INFO("AWS credentials validated successfully");
            valid = 1;
        }
        free(output);
    }

    return valid;
}

int bedrock_authenticate(const char *profile) {
    // Check for custom authentication command first
    const char *custom_auth_cmd = getenv(ENV_AWS_AUTH_COMMAND);
    char command[1024];

    if (custom_auth_cmd && strlen(custom_auth_cmd) > 0) {
        // Use custom authentication command
        LOG_INFO("Using custom authentication command from AWS_AUTH_COMMAND");
        printf("\nAWS credentials not found or expired. Starting authentication...\n");
        printf("Running custom auth command...\n\n");

        snprintf(command, sizeof(command), "%s", custom_auth_cmd);
    } else {
        // Use default AWS SSO login
        if (!profile) {
            profile = getenv(ENV_AWS_PROFILE);
            if (!profile) profile = "default";
        }

        LOG_INFO("Starting AWS SSO login for profile: %s", profile);
        printf("\nAWS credentials not found or expired. Starting authentication...\n");
        printf("Running: aws sso login --profile %s\n\n", profile);

        snprintf(command, sizeof(command), "aws sso login --profile %s", profile);
    }

    int result = system(command);

    if (result == 0) {
        LOG_INFO("Authentication completed successfully");
        printf("\nAuthentication successful! Continuing...\n\n");
        return 0;
    } else {
        LOG_ERROR("Authentication failed with exit code: %d", result);
        printf("\nAuthentication failed. Please check your AWS configuration.\n");
        return -1;
    }
}

char* bedrock_build_endpoint(const char *region, const char *model_id) {
    if (!region || !model_id) {
        return NULL;
    }

    // Bedrock endpoint: https://bedrock-runtime.{region}.amazonaws.com/model/{model-id}/invoke
    size_t len = strlen(region) + strlen(model_id) + 128;
    char *endpoint = malloc(len);
    if (!endpoint) {
        return NULL;
    }

    snprintf(endpoint, len, "https://bedrock-runtime.%s.amazonaws.com/model/%s/invoke",
             region, model_id);

    return endpoint;
}

char* bedrock_convert_request(const char *openai_request) {
    // AWS Bedrock uses Anthropic's native format, not OpenAI format
    // We need to convert from OpenAI to Anthropic format

    cJSON *openai_json = cJSON_Parse(openai_request);
    if (!openai_json) {
        LOG_ERROR("Failed to parse OpenAI request");
        return NULL;
    }

    // Extract necessary fields
    cJSON *messages = cJSON_GetObjectItem(openai_json, "messages");
    cJSON *tools = cJSON_GetObjectItem(openai_json, "tools");
    cJSON *max_tokens = cJSON_GetObjectItem(openai_json, "max_completion_tokens");

    // Build Anthropic format request
    cJSON *anthropic_json = cJSON_CreateObject();

    // Add max_tokens (required by Anthropic)
    if (max_tokens && cJSON_IsNumber(max_tokens)) {
        cJSON_AddNumberToObject(anthropic_json, "max_tokens", max_tokens->valueint);
    } else {
        cJSON_AddNumberToObject(anthropic_json, "max_tokens", 8192);
    }

    // Convert messages from OpenAI to Anthropic format
    cJSON *anthropic_messages = cJSON_CreateArray();
    cJSON *system_prompt = NULL;

    if (messages && cJSON_IsArray(messages)) {
        cJSON *msg = NULL;
        cJSON_ArrayForEach(msg, messages) {
            cJSON *role = cJSON_GetObjectItem(msg, "role");
            cJSON *content = cJSON_GetObjectItem(msg, "content");

            if (!role || !cJSON_IsString(role)) continue;

            const char *role_str = role->valuestring;

            // Extract system message separately
            if (strcmp(role_str, "system") == 0) {
                if (cJSON_IsString(content)) {
                    system_prompt = cJSON_CreateString(content->valuestring);
                } else if (cJSON_IsArray(content)) {
                    // Extract text from content array
                    cJSON *item = cJSON_GetArrayItem(content, 0);
                    if (item) {
                        cJSON *text = cJSON_GetObjectItem(item, "text");
                        if (text && cJSON_IsString(text)) {
                            system_prompt = cJSON_CreateString(text->valuestring);
                        }
                    }
                }
                continue;
            }

            // Convert user/assistant messages
            cJSON *anthropic_msg = cJSON_CreateObject();

            // Map roles
            if (strcmp(role_str, "assistant") == 0) {
                cJSON_AddStringToObject(anthropic_msg, "role", "assistant");

                // Handle tool calls
                cJSON *tool_calls = cJSON_GetObjectItem(msg, "tool_calls");
                if (tool_calls && cJSON_IsArray(tool_calls)) {
                    cJSON *content_array = cJSON_CreateArray();

                    // Add text content if present
                    if (cJSON_IsString(content) && strlen(content->valuestring) > 0) {
                        cJSON *text_block = cJSON_CreateObject();
                        cJSON_AddStringToObject(text_block, "type", "text");
                        cJSON_AddStringToObject(text_block, "text", content->valuestring);
                        cJSON_AddItemToArray(content_array, text_block);
                    }

                    // Add tool use blocks
                    cJSON *tool_call = NULL;
                    cJSON_ArrayForEach(tool_call, tool_calls) {
                        cJSON *tool_use_block = cJSON_CreateObject();
                        cJSON_AddStringToObject(tool_use_block, "type", "tool_use");

                        cJSON *id = cJSON_GetObjectItem(tool_call, "id");
                        if (id && cJSON_IsString(id)) {
                            cJSON_AddStringToObject(tool_use_block, "id", id->valuestring);
                        }

                        cJSON *function = cJSON_GetObjectItem(tool_call, "function");
                        if (function) {
                            cJSON *name = cJSON_GetObjectItem(function, "name");
                            cJSON *arguments = cJSON_GetObjectItem(function, "arguments");

                            if (name && cJSON_IsString(name)) {
                                cJSON_AddStringToObject(tool_use_block, "name", name->valuestring);
                            }

                            if (arguments && cJSON_IsString(arguments)) {
                                cJSON *input = cJSON_Parse(arguments->valuestring);
                                if (input) {
                                    cJSON_AddItemToObject(tool_use_block, "input", input);
                                }
                            }
                        }

                        cJSON_AddItemToArray(content_array, tool_use_block);
                    }

                    cJSON_AddItemToObject(anthropic_msg, "content", content_array);
                } else {
                    // Simple text content
                    if (cJSON_IsString(content)) {
                        cJSON_AddStringToObject(anthropic_msg, "content", content->valuestring);
                    } else if (cJSON_IsNull(content)) {
                        cJSON_AddStringToObject(anthropic_msg, "content", "");
                    }
                }
            } else if (strcmp(role_str, "user") == 0) {
                cJSON_AddStringToObject(anthropic_msg, "role", "user");

                if (cJSON_IsString(content)) {
                    cJSON_AddStringToObject(anthropic_msg, "content", content->valuestring);
                } else if (cJSON_IsArray(content)) {
                    cJSON_AddItemToObject(anthropic_msg, "content", cJSON_Duplicate(content, 1));
                }
            } else if (strcmp(role_str, "tool") == 0) {
                // Tool results are handled as user messages with tool_result blocks
                cJSON *tool_call_id = cJSON_GetObjectItem(msg, "tool_call_id");

                if (tool_call_id && cJSON_IsString(tool_call_id)) {
                    cJSON *tool_msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(tool_msg, "role", "user");

                    cJSON *content_array = cJSON_CreateArray();
                    cJSON *tool_result_block = cJSON_CreateObject();
                    cJSON_AddStringToObject(tool_result_block, "type", "tool_result");
                    cJSON_AddStringToObject(tool_result_block, "tool_use_id", tool_call_id->valuestring);

                    if (cJSON_IsString(content)) {
                        // Try to parse as JSON first
                        cJSON *parsed = cJSON_Parse(content->valuestring);
                        if (parsed) {
                            cJSON_AddItemToObject(tool_result_block, "content", parsed);
                        } else {
                            cJSON_AddStringToObject(tool_result_block, "content", content->valuestring);
                        }
                    }

                    cJSON_AddItemToArray(content_array, tool_result_block);
                    cJSON_AddItemToObject(tool_msg, "content", content_array);
                    cJSON_AddItemToArray(anthropic_messages, tool_msg);
                }
                continue;
            }

            if (anthropic_msg && cJSON_GetObjectItem(anthropic_msg, "role")) {
                cJSON_AddItemToArray(anthropic_messages, anthropic_msg);
            } else {
                cJSON_Delete(anthropic_msg);
            }
        }
    }

    cJSON_AddItemToObject(anthropic_json, "messages", anthropic_messages);

    // Add system prompt if present
    if (system_prompt) {
        cJSON_AddItemToObject(anthropic_json, "system", system_prompt);
    }

    // Convert tools to Anthropic format
    if (tools && cJSON_IsArray(tools)) {
        cJSON *anthropic_tools = cJSON_CreateArray();

        cJSON *tool = NULL;
        cJSON_ArrayForEach(tool, tools) {
            cJSON *function = cJSON_GetObjectItem(tool, "function");
            if (function) {
                cJSON *anthropic_tool = cJSON_CreateObject();

                cJSON *name = cJSON_GetObjectItem(function, "name");
                cJSON *description = cJSON_GetObjectItem(function, "description");
                cJSON *parameters = cJSON_GetObjectItem(function, "parameters");

                if (name && cJSON_IsString(name)) {
                    cJSON_AddStringToObject(anthropic_tool, "name", name->valuestring);
                }
                if (description && cJSON_IsString(description)) {
                    cJSON_AddStringToObject(anthropic_tool, "description", description->valuestring);
                }
                if (parameters) {
                    cJSON_AddItemToObject(anthropic_tool, "input_schema", cJSON_Duplicate(parameters, 1));
                }

                cJSON_AddItemToArray(anthropic_tools, anthropic_tool);
            }
        }

        if (cJSON_GetArraySize(anthropic_tools) > 0) {
            cJSON_AddItemToObject(anthropic_json, "tools", anthropic_tools);
        } else {
            cJSON_Delete(anthropic_tools);
        }
    }

    // Add anthropic_version
    cJSON_AddStringToObject(anthropic_json, "anthropic_version", "bedrock-2023-05-31");

    char *result = cJSON_PrintUnformatted(anthropic_json);

    cJSON_Delete(openai_json);
    cJSON_Delete(anthropic_json);

    return result;
}

cJSON* bedrock_convert_response(const char *bedrock_response) {
    // AWS Bedrock returns Anthropic's native format
    // We need to convert it to OpenAI format

    cJSON *anthropic_json = cJSON_Parse(bedrock_response);
    if (!anthropic_json) {
        LOG_ERROR("Failed to parse Bedrock response");
        return NULL;
    }

    // Build OpenAI format response
    cJSON *openai_json = cJSON_CreateObject();

    // Add id (generate if not present)
    cJSON *id = cJSON_GetObjectItem(anthropic_json, "id");
    if (id && cJSON_IsString(id)) {
        cJSON_AddStringToObject(openai_json, "id", id->valuestring);
    } else {
        cJSON_AddStringToObject(openai_json, "id", "bedrock-request");
    }

    // Add object type
    cJSON_AddStringToObject(openai_json, "object", "chat.completion");

    // Add created timestamp
    time_t now = time(NULL);
    cJSON_AddNumberToObject(openai_json, "created", (double)now);

    // Add model
    cJSON *model = cJSON_GetObjectItem(anthropic_json, "model");
    if (model && cJSON_IsString(model)) {
        cJSON_AddStringToObject(openai_json, "model", model->valuestring);
    } else {
        cJSON_AddStringToObject(openai_json, "model", "claude-bedrock");
    }

    // Add choices array
    cJSON *choices = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON_AddNumberToObject(choice, "index", 0);

    // Add message
    cJSON *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "assistant");

    // Convert content blocks
    cJSON *content = cJSON_GetObjectItem(anthropic_json, "content");
    cJSON *tool_calls = NULL;
    char *text_content = NULL;

    if (content && cJSON_IsArray(content)) {
        cJSON *block = NULL;
        cJSON_ArrayForEach(block, content) {
            cJSON *type = cJSON_GetObjectItem(block, "type");
            if (!type || !cJSON_IsString(type)) continue;

            const char *type_str = type->valuestring;

            if (strcmp(type_str, "text") == 0) {
                cJSON *text = cJSON_GetObjectItem(block, "text");
                if (text && cJSON_IsString(text)) {
                    text_content = text->valuestring;
                }
            } else if (strcmp(type_str, "tool_use") == 0) {
                if (!tool_calls) {
                    tool_calls = cJSON_CreateArray();
                }

                cJSON *tool_call = cJSON_CreateObject();

                cJSON *tool_id = cJSON_GetObjectItem(block, "id");
                if (tool_id && cJSON_IsString(tool_id)) {
                    cJSON_AddStringToObject(tool_call, "id", tool_id->valuestring);
                }

                cJSON_AddStringToObject(tool_call, "type", "function");

                cJSON *function = cJSON_CreateObject();
                cJSON *name = cJSON_GetObjectItem(block, "name");
                if (name && cJSON_IsString(name)) {
                    cJSON_AddStringToObject(function, "name", name->valuestring);
                }

                cJSON *input = cJSON_GetObjectItem(block, "input");
                if (input) {
                    char *input_str = cJSON_PrintUnformatted(input);
                    cJSON_AddStringToObject(function, "arguments", input_str);
                    free(input_str);
                }

                cJSON_AddItemToObject(tool_call, "function", function);
                cJSON_AddItemToArray(tool_calls, tool_call);
            }
        }
    }

    // Add content to message
    if (text_content) {
        cJSON_AddStringToObject(message, "content", text_content);
    } else {
        cJSON_AddNullToObject(message, "content");
    }

    // Add tool_calls if present
    if (tool_calls) {
        cJSON_AddItemToObject(message, "tool_calls", tool_calls);
    }

    cJSON_AddItemToObject(choice, "message", message);

    // Add finish_reason
    cJSON *stop_reason = cJSON_GetObjectItem(anthropic_json, "stop_reason");
    if (stop_reason && cJSON_IsString(stop_reason)) {
        const char *reason = stop_reason->valuestring;
        if (strcmp(reason, "end_turn") == 0) {
            cJSON_AddStringToObject(choice, "finish_reason", "stop");
        } else if (strcmp(reason, "tool_use") == 0) {
            cJSON_AddStringToObject(choice, "finish_reason", "tool_calls");
        } else if (strcmp(reason, "max_tokens") == 0) {
            cJSON_AddStringToObject(choice, "finish_reason", "length");
        } else {
            cJSON_AddStringToObject(choice, "finish_reason", reason);
        }
    } else {
        cJSON_AddStringToObject(choice, "finish_reason", "stop");
    }

    cJSON_AddItemToArray(choices, choice);
    cJSON_AddItemToObject(openai_json, "choices", choices);

    // Add usage if present
    cJSON *usage_anthropic = cJSON_GetObjectItem(anthropic_json, "usage");
    if (usage_anthropic) {
        cJSON *usage = cJSON_CreateObject();

        cJSON *input_tokens = cJSON_GetObjectItem(usage_anthropic, "input_tokens");
        cJSON *output_tokens = cJSON_GetObjectItem(usage_anthropic, "output_tokens");

        if (input_tokens && cJSON_IsNumber(input_tokens)) {
            cJSON_AddNumberToObject(usage, "prompt_tokens", input_tokens->valueint);
        }
        if (output_tokens && cJSON_IsNumber(output_tokens)) {
            cJSON_AddNumberToObject(usage, "completion_tokens", output_tokens->valueint);
        }

        int total = 0;
        if (input_tokens && cJSON_IsNumber(input_tokens)) total += input_tokens->valueint;
        if (output_tokens && cJSON_IsNumber(output_tokens)) total += output_tokens->valueint;
        cJSON_AddNumberToObject(usage, "total_tokens", total);

        cJSON_AddItemToObject(openai_json, "usage", usage);
    }

    cJSON_Delete(anthropic_json);
    return openai_json;
}

struct curl_slist* bedrock_sign_request(
    struct curl_slist *headers,
    const char *method,
    const char *url,
    const char *payload,
    const AWSCredentials *creds,
    const char *region,
    const char *service
) {
    if (!method || !url || !payload || !creds || !region || !service) {
        LOG_ERROR("Invalid parameters for bedrock_sign_request");
        return NULL;
    }

    // Get timestamps
    char *timestamp = get_iso8601_timestamp();
    char *datestamp = get_date_stamp();

    if (!timestamp || !datestamp) {
        free(timestamp);
        free(datestamp);
        return NULL;
    }

    // Parse URL to extract host and path
    const char *host_start = strstr(url, "://");
    if (!host_start) {
        free(timestamp);
        free(datestamp);
        return NULL;
    }
    host_start += 3;  // Skip "://"

    const char *path_start = strchr(host_start, '/');
    char *host = NULL;
    char *path = NULL;

    if (path_start) {
        size_t host_len = (size_t)(path_start - host_start);
        host = malloc(host_len + 1);
        if (host) {
            memcpy(host, host_start, host_len);
            host[host_len] = '\0';
        }
        path = strdup(path_start);
    } else {
        host = strdup(host_start);
        path = strdup("/");
    }

    if (!host || !path) {
        free(timestamp);
        free(datestamp);
        free(host);
        free(path);
        return NULL;
    }

    // Create canonical request
    char *payload_hash = sha256_hash(payload);

    // URL-encode the path for canonical request (per AWS SigV4 spec)
    char *encoded_path = url_encode(path, 0);  // 0 = don't encode slashes
    if (!encoded_path) {
        free(timestamp);
        free(datestamp);
        free(host);
        free(path);
        free(payload_hash);
        return NULL;
    }

    // Canonical headers (must be sorted)
    char canonical_headers[2048];
    snprintf(canonical_headers, sizeof(canonical_headers),
             "host:%s\nx-amz-date:%s\n",
             host, timestamp);

    const char *signed_headers = "host;x-amz-date";

    // Build canonical request
    char canonical_request[8192];
    snprintf(canonical_request, sizeof(canonical_request),
             "%s\n%s\n\n%s\n%s\n%s",
             method, encoded_path, canonical_headers, signed_headers, payload_hash);

    char *canonical_request_hash = sha256_hash(canonical_request);

    // Create string to sign
    char string_to_sign[1024];
    snprintf(string_to_sign, sizeof(string_to_sign),
             "AWS4-HMAC-SHA256\n%s\n%s/%s/%s/aws4_request\n%s",
             timestamp, datestamp, region, service, canonical_request_hash);

    // Calculate signing key
    char key_buffer[256];
    snprintf(key_buffer, sizeof(key_buffer), "AWS4%s", creds->secret_access_key);

    unsigned char k_date[32];
    hmac_sha256((const unsigned char*)key_buffer, strlen(key_buffer),
                (const unsigned char*)datestamp, strlen(datestamp), k_date);

    unsigned char k_region[32];
    hmac_sha256(k_date, 32, (const unsigned char*)region, strlen(region), k_region);

    unsigned char k_service[32];
    hmac_sha256(k_region, 32, (const unsigned char*)service, strlen(service), k_service);

    unsigned char signing_key[32];
    hmac_sha256(k_service, 32, (const unsigned char*)"aws4_request", 12, signing_key);

    // Calculate signature
    unsigned char signature_bytes[32];
    hmac_sha256(signing_key, 32,
                (unsigned char*)string_to_sign, strlen(string_to_sign),
                signature_bytes);

    char *signature = hex_encode(signature_bytes, 32);

    // Build authorization header
    char auth_header[2048];
    snprintf(auth_header, sizeof(auth_header),
             "Authorization: AWS4-HMAC-SHA256 Credential=%s/%s/%s/%s/aws4_request, SignedHeaders=%s, Signature=%s",
             creds->access_key_id, datestamp, region, service,
             signed_headers, signature);

    // Add headers
    char date_header[128];
    snprintf(date_header, sizeof(date_header), "x-amz-date: %s", timestamp);

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, date_header);
    headers = curl_slist_append(headers, auth_header);

    // Add session token if present
    if (creds->session_token) {
        char token_header[512];
        snprintf(token_header, sizeof(token_header), "x-amz-security-token: %s", creds->session_token);
        headers = curl_slist_append(headers, token_header);
    }

    // Cleanup
    free(timestamp);
    free(datestamp);
    free(host);
    free(path);
    free(encoded_path);
    free(payload_hash);
    free(canonical_request_hash);
    free(signature);

    return headers;
}
