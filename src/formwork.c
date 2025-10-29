/**
 * FormWork - C implementation
 */

#include "formwork.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdarg.h>

// ============================================================================
// Internal helpers
// ============================================================================

static char* string_dup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *dup = malloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, str, len + 1);
    return dup;
}

static char* string_format(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    // Calculate required size
    va_list args_copy;
    va_copy(args_copy, args);
    int size = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (size < 0) {
        va_end(args);
        return NULL;
    }

    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        va_end(args);
        return NULL;
    }

    vsnprintf(buffer, (size_t)size + 1, fmt, args);
    va_end(args);

    return buffer;
}

// Strip leading/trailing whitespace (modifies in place, returns start)
static char* strip_whitespace(char *str) {
    if (!str) return NULL;

    // Strip leading whitespace
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }

    // Strip trailing whitespace
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return str;
}

// ============================================================================
// Public API Implementation
// ============================================================================

void formwork_config_init(FormWorkConfig *config) {
    if (!config) return;

    memset(config, 0, sizeof(FormWorkConfig));
    config->max_retries = FORMWORK_DEFAULT_MAX_RETRIES;
    config->retry_delay_ms = FORMWORK_DEFAULT_RETRY_DELAY_MS;
}

const char* formwork_error_string(FormWorkError error) {
    switch (error) {
        case FORMWORK_SUCCESS:
            return "Success";
        case FORMWORK_ERROR_INVALID_JSON:
            return "Invalid JSON in LLM response";
        case FORMWORK_ERROR_EMPTY_RESPONSE:
            return "Empty response from LLM";
        case FORMWORK_ERROR_MAX_RETRIES:
            return "Maximum retries exceeded";
        case FORMWORK_ERROR_CALLBACK_FAILED:
            return "Callback function failed";
        case FORMWORK_ERROR_ALLOCATION_FAILED:
            return "Memory allocation failed";
        case FORMWORK_ERROR_INVALID_CONFIG:
            return "Invalid configuration";
        default:
            return "Unknown error";
    }
}

char* formwork_build_prompt(const FormWorkConfig *config) {
    if (!config || !config->base_prompt || !config->target_name) {
        return NULL;
    }

    // Start building the prompt
    size_t capacity = 4096;
    char *prompt = malloc(capacity);
    if (!prompt) return NULL;

    size_t offset = 0;

    // Add base prompt
    int written = snprintf(prompt + offset, capacity - offset,
                          "%s\n\n# Output format\n"
                          "Your response MUST be a valid JSON string that matches this exact schema:\n\n",
                          config->base_prompt);

    if (written < 0 || (size_t)written >= capacity - offset) {
        // Buffer too small, reallocate
        capacity *= 2;
        char *new_prompt = realloc(prompt, capacity);
        if (!new_prompt) {
            free(prompt);
            return NULL;
        }
        prompt = new_prompt;

        written = snprintf(prompt + offset, capacity - offset,
                          "%s\n\n# Output format\n"
                          "Your response MUST be a valid JSON string that matches this exact schema:\n\n",
                          config->base_prompt);

        if (written < 0 || (size_t)written >= capacity - offset) {
            free(prompt);
            return NULL;
        }
    }
    offset += (size_t)written;

    // Add JSON schema if provided
    if (config->json_schema) {
        written = snprintf(prompt + offset, capacity - offset,
                          "JSON Schema for %s:\n%s\n\n",
                          config->target_name, config->json_schema);

        if (written < 0 || (size_t)written >= capacity - offset) {
            // Buffer too small, reallocate
            capacity = capacity * 2 + strlen(config->json_schema);
            char *new_prompt = realloc(prompt, capacity);
            if (!new_prompt) {
                free(prompt);
                return NULL;
            }
            prompt = new_prompt;

            written = snprintf(prompt + offset, capacity - offset,
                              "JSON Schema for %s:\n%s\n\n",
                              config->target_name, config->json_schema);

            if (written < 0 || (size_t)written >= capacity - offset) {
                free(prompt);
                return NULL;
            }
        }
    }

    return prompt;
}

char* formwork_build_retry_prompt(const FormWorkConfig *config,
                                   const char *last_error,
                                   const char *last_response) {
    if (!config || !config->base_prompt || !config->target_name || !last_error) {
        return NULL;
    }

    size_t capacity = 8192;
    if (last_response) {
        capacity += strlen(last_response);
    }

    char *prompt = malloc(capacity);
    if (!prompt) return NULL;

    size_t offset = 0;

    // Original request
    int written = snprintf(prompt + offset, capacity - offset,
                          "<original_request>\n%s\n</original_request>\n\n",
                          config->base_prompt);
    if (written < 0 || (size_t)written >= capacity - offset) goto error;
    offset += (size_t)written;

    // Error message
    written = snprintf(prompt + offset, capacity - offset,
                      "<error>\nYour previous response failed with this error:\n%s\n</error>\n\n",
                      last_error);
    if (written < 0 || (size_t)written >= capacity - offset) goto error;
    offset += (size_t)written;

    // Previous response (if available)
    if (last_response && last_response[0]) {
        written = snprintf(prompt + offset, capacity - offset,
                          "<previous_response>\n%s\n</previous_response>\n\n",
                          last_response);
        if (written < 0 || (size_t)written >= capacity - offset) goto error;
        offset += (size_t)written;
    }

    // Instructions
    written = snprintf(prompt + offset, capacity - offset,
                      "<instructions>\n"
                      "CRITICAL: Carefully review the desired output format in the <original_request>. "
                      "Fix the specific error mentioned above. "
                      "Return ONLY valid JSON that can be parsed into a %s object. "
                      "Do not include explanations, markdown formatting, or additional text."
                      "\n</instructions>",
                      config->target_name);
    if (written < 0 || (size_t)written >= capacity - offset) goto error;

    return prompt;

error:
    free(prompt);
    return NULL;
}

cJSON* formwork_extract_json(const char *llm_output) {
    if (!llm_output || !llm_output[0]) {
        return NULL;
    }

    // Make a working copy
    char *working = string_dup(llm_output);
    if (!working) return NULL;

    // Strip leading/trailing whitespace
    char *start = strip_whitespace(working);

    // Remove markdown code fences
    if (strncmp(start, "```json", 7) == 0) {
        start += 7;
    } else if (strncmp(start, "```", 3) == 0) {
        start += 3;
    }

    // Strip whitespace after fence
    start = strip_whitespace(start);

    // Remove trailing code fence
    size_t len = strlen(start);
    if (len >= 3 && strcmp(start + len - 3, "```") == 0) {
        start[len - 3] = '\0';
        start = strip_whitespace(start);
        len = strlen(start);
    }

    // Find first JSON bracket
    char *json_start = NULL;
    char open_char = 0, close_char = 0;

    for (size_t i = 0; i < len; i++) {
        if (start[i] == '{') {
            json_start = start + i;
            open_char = '{';
            close_char = '}';
            break;
        } else if (start[i] == '[') {
            json_start = start + i;
            open_char = '[';
            close_char = ']';
            break;
        }
    }

    if (!json_start) {
        free(working);
        return NULL;
    }

    // Find matching closing bracket
    int depth = 0;
    char *json_end = NULL;

    for (char *p = json_start; *p; p++) {
        if (*p == open_char) {
            depth++;
        } else if (*p == close_char) {
            depth--;
            if (depth == 0) {
                json_end = p + 1;
                break;
            }
        }
    }

    if (!json_end || depth != 0) {
        free(working);
        return NULL;
    }

    // Temporarily null-terminate at end of JSON
    char saved = *json_end;
    *json_end = '\0';

    // Parse JSON
    cJSON *json = cJSON_Parse(json_start);

    // Restore character (though we're about to free anyway)
    *json_end = saved;

    free(working);
    return json;
}

FormWorkResult formwork_construct(const FormWorkConfig *config) {
    FormWorkResult result = {0};

    // Validate config
    if (!config || !config->target_name || !config->base_prompt || !config->llm_caller) {
        result.error_code = FORMWORK_ERROR_INVALID_CONFIG;
        result.error_message = string_dup("Invalid configuration: missing required fields");
        return result;
    }

    char *last_error = NULL;
    char *last_response = NULL;

    int max_retries = config->max_retries > 0 ? config->max_retries : FORMWORK_DEFAULT_MAX_RETRIES;
    long retry_delay_ms = config->retry_delay_ms > 0 ? config->retry_delay_ms : FORMWORK_DEFAULT_RETRY_DELAY_MS;

    for (int attempt = 1; attempt <= max_retries; attempt++) {
        result.attempts_used = attempt;

        // Notify metrics
        if (config->metrics && config->metrics->on_attempt_start) {
            config->metrics->on_attempt_start(config->target_name, attempt, max_retries,
                                              config->metrics->user_data);
        }

        // Build prompt
        char *prompt;
        if (attempt == 1) {
            prompt = formwork_build_prompt(config);
        } else {
            prompt = formwork_build_retry_prompt(config, last_error, last_response);
        }

        if (!prompt) {
            result.error_code = FORMWORK_ERROR_ALLOCATION_FAILED;
            result.error_message = string_dup("Failed to build prompt");
            break;
        }

        // Call LLM
        char *llm_response = config->llm_caller(prompt, config->llm_user_data);
        free(prompt);

        if (!llm_response || !llm_response[0]) {
            free(llm_response);

            free(last_error);
            last_error = string_dup("LLM returned empty response");

            // Notify error callback
            if (config->error_callback) {
                config->error_callback(FORMWORK_ERROR_EMPTY_RESPONSE, last_error,
                                      config->error_user_data);
            }

            // Retry or fail
            if (attempt < max_retries) {
                if (config->metrics && config->metrics->on_attempt_retry) {
                    config->metrics->on_attempt_retry(config->target_name, attempt, max_retries,
                                                      last_error, config->metrics->user_data);
                }
                usleep((useconds_t)(retry_delay_ms * 1000));
                continue;
            } else {
                result.error_code = FORMWORK_ERROR_EMPTY_RESPONSE;
                result.error_message = string_dup(last_error);
                break;
            }
        }

        // Save response for debugging
        free(last_response);
        last_response = string_dup(llm_response);

        // Extract JSON
        cJSON *json = formwork_extract_json(llm_response);
        free(llm_response);

        if (!json) {
            free(last_error);
            last_error = string_format("Failed to extract valid JSON from LLM response");

            // Notify error callback
            if (config->error_callback) {
                config->error_callback(FORMWORK_ERROR_INVALID_JSON, last_error,
                                      config->error_user_data);
            }

            // Retry or fail
            if (attempt < max_retries) {
                if (config->metrics && config->metrics->on_attempt_retry) {
                    config->metrics->on_attempt_retry(config->target_name, attempt, max_retries,
                                                      last_error, config->metrics->user_data);
                }
                usleep((useconds_t)(retry_delay_ms * 1000));
                continue;
            } else {
                result.error_code = FORMWORK_ERROR_INVALID_JSON;
                result.error_message = string_dup(last_error);
                break;
            }
        }

        // Success!
        result.json = json;
        result.error_code = FORMWORK_SUCCESS;
        result.last_llm_response = last_response;
        last_response = NULL;  // Transfer ownership

        if (config->metrics && config->metrics->on_attempt_success) {
            config->metrics->on_attempt_success(config->target_name, attempt, max_retries,
                                                config->metrics->user_data);
        }

        free(last_error);
        return result;
    }

    // All attempts failed
    if (result.error_code != FORMWORK_SUCCESS) {
        if (config->metrics && config->metrics->on_final_failure) {
            config->metrics->on_final_failure(config->target_name, result.attempts_used,
                                              result.error_message, config->metrics->user_data);
        }

        result.last_llm_response = last_response;
        last_response = NULL;  // Transfer ownership
    }

    free(last_error);
    free(last_response);

    return result;
}

void formwork_result_free(FormWorkResult *result) {
    if (!result) return;

    if (result->json) {
        cJSON_Delete(result->json);
        result->json = NULL;
    }

    free(result->error_message);
    result->error_message = NULL;

    free(result->last_llm_response);
    result->last_llm_response = NULL;
}

char* formwork_build_simple_schema(const char *type_name,
                                    const char *fields[][2],
                                    size_t field_count) {
    if (!type_name || !fields || field_count == 0) {
        return NULL;
    }

    cJSON *schema = cJSON_CreateObject();
    if (!schema) return NULL;

    cJSON_AddStringToObject(schema, "type", "object");

    cJSON *properties = cJSON_CreateObject();
    if (!properties) {
        cJSON_Delete(schema);
        return NULL;
    }
    cJSON_AddItemToObject(schema, "properties", properties);

    cJSON *required = cJSON_CreateArray();
    if (!required) {
        cJSON_Delete(schema);
        return NULL;
    }
    cJSON_AddItemToObject(schema, "required", required);

    for (size_t i = 0; i < field_count; i++) {
        const char *field_name = fields[i][0];
        const char *field_type = fields[i][1];

        cJSON *field_schema = cJSON_CreateObject();
        if (!field_schema) {
            cJSON_Delete(schema);
            return NULL;
        }

        cJSON_AddStringToObject(field_schema, "type", field_type);
        cJSON_AddItemToObject(properties, field_name, field_schema);

        cJSON *required_item = cJSON_CreateString(field_name);
        if (!required_item) {
            cJSON_Delete(schema);
            return NULL;
        }
        cJSON_AddItemToArray(required, required_item);
    }

    char *schema_str = cJSON_Print(schema);
    cJSON_Delete(schema);

    return schema_str;
}
