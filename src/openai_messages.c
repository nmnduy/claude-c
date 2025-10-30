/*
 * openai_messages.c - OpenAI message format conversion
 */

#define _POSIX_C_SOURCE 200809L

#include "openai_messages.h"
#include "logger.h"
#include "claude_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Build OpenAI request JSON from internal message format
 */
cJSON* build_openai_request(ConversationState *state, int enable_caching) {
    if (!state) {
        LOG_ERROR("ConversationState is NULL");
        return NULL;
    }

    LOG_DEBUG("Building OpenAI request (messages: %d, caching: %s)",
              state->count, enable_caching ? "enabled" : "disabled");

    cJSON *request = cJSON_CreateObject();
    if (!request) return NULL;

    cJSON_AddStringToObject(request, "model", state->model);
    cJSON_AddNumberToObject(request, "max_completion_tokens", MAX_TOKENS);

    cJSON *messages_array = cJSON_CreateArray();
    if (!messages_array) {
        cJSON_Delete(request);
        return NULL;
    }

    // Convert each internal message to OpenAI format
    for (int i = 0; i < state->count; i++) {
        InternalMessage *msg = &state->messages[i];

        // Determine if this is a recent message (for cache breakpoints)
        int is_recent_message = (i >= state->count - 3) && enable_caching;

        if (msg->role == MSG_SYSTEM) {
            // System messages
            cJSON *sys_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(sys_msg, "role", "system");

            // Find text content
            for (int j = 0; j < msg->content_count; j++) {
                InternalContent *c = &msg->contents[j];
                if (c->type == INTERNAL_TEXT && c->text) {
                    // Use content array for cache_control support
                    if (enable_caching) {
                        cJSON *content_array = cJSON_CreateArray();
                        cJSON *text_block = cJSON_CreateObject();
                        cJSON_AddStringToObject(text_block, "type", "text");
                        cJSON_AddStringToObject(text_block, "text", c->text);
                        add_cache_control(text_block);
                        cJSON_AddItemToArray(content_array, text_block);
                        cJSON_AddItemToObject(sys_msg, "content", content_array);
                    } else {
                        cJSON_AddStringToObject(sys_msg, "content", c->text);
                    }
                    break;
                }
            }

            cJSON_AddItemToArray(messages_array, sys_msg);
        }
        else if (msg->role == MSG_USER) {
            // User messages - may contain text or tool responses
            for (int j = 0; j < msg->content_count; j++) {
                InternalContent *c = &msg->contents[j];

                if (c->type == INTERNAL_TEXT && c->text) {
                    // Regular user text
                    cJSON *user_msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(user_msg, "role", "user");

                    // Use content array for recent messages to support cache_control
                    if (is_recent_message && i == state->count - 1) {
                        cJSON *content_array = cJSON_CreateArray();
                        cJSON *text_block = cJSON_CreateObject();
                        cJSON_AddStringToObject(text_block, "type", "text");
                        cJSON_AddStringToObject(text_block, "text", c->text);
                        add_cache_control(text_block);
                        cJSON_AddItemToArray(content_array, text_block);
                        cJSON_AddItemToObject(user_msg, "content", content_array);
                    } else {
                        cJSON_AddStringToObject(user_msg, "content", c->text);
                    }

                    cJSON_AddItemToArray(messages_array, user_msg);
                }
                else if (c->type == INTERNAL_TOOL_RESPONSE) {
                    // Tool response - OpenAI uses "tool" role
                    cJSON *tool_msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(tool_msg, "role", "tool");
                    cJSON_AddStringToObject(tool_msg, "tool_call_id", c->tool_id);

                    // Convert output to string
                    char *output_str = cJSON_PrintUnformatted(c->tool_output);
                    cJSON_AddStringToObject(tool_msg, "content", output_str ? output_str : "{}");
                    free(output_str);

                    cJSON_AddItemToArray(messages_array, tool_msg);
                }
            }
        }
        else if (msg->role == MSG_ASSISTANT) {
            // Assistant messages - may contain text and/or tool calls
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");

            // Collect text content
            char *text_content = NULL;
            for (int j = 0; j < msg->content_count; j++) {
                InternalContent *c = &msg->contents[j];
                if (c->type == INTERNAL_TEXT && c->text) {
                    text_content = c->text;
                    break;
                }
            }

            // Collect tool calls
            cJSON *tool_calls = NULL;
            for (int j = 0; j < msg->content_count; j++) {
                InternalContent *c = &msg->contents[j];
                if (c->type == INTERNAL_TOOL_CALL) {
                    if (!tool_calls) {
                        tool_calls = cJSON_CreateArray();
                    }

                    cJSON *tc = cJSON_CreateObject();
                    cJSON_AddStringToObject(tc, "id", c->tool_id);
                    cJSON_AddStringToObject(tc, "type", "function");

                    cJSON *func = cJSON_CreateObject();
                    cJSON_AddStringToObject(func, "name", c->tool_name);

                    char *args_str = cJSON_PrintUnformatted(c->tool_params);
                    cJSON_AddStringToObject(func, "arguments", args_str ? args_str : "{}");
                    free(args_str);

                    cJSON_AddItemToObject(tc, "function", func);
                    cJSON_AddItemToArray(tool_calls, tc);
                }
            }

            // Add content (required field in OpenAI API)
            if (text_content) {
                cJSON_AddStringToObject(asst_msg, "content", text_content);
            } else {
                cJSON_AddNullToObject(asst_msg, "content");
            }

            // Add tool_calls if present
            if (tool_calls) {
                cJSON_AddItemToObject(asst_msg, "tool_calls", tool_calls);
            } else {
                cJSON_Delete(tool_calls);  // Free if empty
            }

            cJSON_AddItemToArray(messages_array, asst_msg);
        }
    }

    cJSON_AddItemToObject(request, "messages", messages_array);

    // Add tools with cache_control support
    cJSON *tool_defs = get_tool_definitions(enable_caching);
    cJSON_AddItemToObject(request, "tools", tool_defs);

    LOG_DEBUG("OpenAI request built successfully");
    return request;
}

/**
 * Parse OpenAI response into internal message format
 */
InternalMessage parse_openai_response(cJSON *response) {
    InternalMessage msg = {0};
    msg.role = MSG_ASSISTANT;

    if (!response) {
        LOG_ERROR("Response is NULL");
        return msg;
    }

    cJSON *choices = cJSON_GetObjectItem(response, "choices");
    if (!choices || !cJSON_IsArray(choices)) {
        LOG_ERROR("Invalid response: missing 'choices' array");
        return msg;
    }

    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    if (!choice) {
        LOG_ERROR("Invalid response: empty 'choices' array");
        return msg;
    }

    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!message) {
        LOG_ERROR("Invalid response: missing 'message' object");
        return msg;
    }

    // Count content blocks
    int count = 0;
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content) && content->valuestring) {
        count++;
    }

    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        count += cJSON_GetArraySize(tool_calls);
    }

    if (count == 0) {
        LOG_WARN("Response has no content or tool_calls");
        return msg;
    }

    // Allocate content array
    msg.contents = calloc((size_t)count, sizeof(InternalContent));
    if (!msg.contents) {
        LOG_ERROR("Failed to allocate content array");
        return msg;
    }
    msg.content_count = count;

    int idx = 0;

    // Parse text content
    if (content && cJSON_IsString(content) && content->valuestring) {
        msg.contents[idx].type = INTERNAL_TEXT;
        msg.contents[idx].text = strdup(content->valuestring);
        if (!msg.contents[idx].text) {
            LOG_ERROR("Failed to duplicate text content");
        }
        idx++;
    }

    // Parse tool calls
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        cJSON *tc = NULL;
        cJSON_ArrayForEach(tc, tool_calls) {
            if (idx >= count) break;

            cJSON *id = cJSON_GetObjectItem(tc, "id");
            cJSON *func = cJSON_GetObjectItem(tc, "function");
            if (!func) continue;

            cJSON *name = cJSON_GetObjectItem(func, "name");
            cJSON *arguments = cJSON_GetObjectItem(func, "arguments");

            if (!id || !name || !arguments) {
                LOG_WARN("Malformed tool_call, skipping");
                continue;
            }

            msg.contents[idx].type = INTERNAL_TOOL_CALL;

            // Copy tool_id
            msg.contents[idx].tool_id = strdup(id->valuestring);

            // Copy tool_name
            msg.contents[idx].tool_name = strdup(name->valuestring);

            // Parse arguments JSON string to cJSON object
            const char *args_str = arguments->valuestring;
            msg.contents[idx].tool_params = cJSON_Parse(args_str ? args_str : "{}");
            if (!msg.contents[idx].tool_params) {
                LOG_WARN("Failed to parse tool arguments, using empty object");
                msg.contents[idx].tool_params = cJSON_CreateObject();
            }

            idx++;
        }
    }

    LOG_DEBUG("Parsed OpenAI response: %d content blocks", msg.content_count);
    return msg;
}

/**
 * Free internal message contents
 */
void free_internal_message(InternalMessage *msg) {
    if (!msg || !msg->contents) return;

    for (int i = 0; i < msg->content_count; i++) {
        InternalContent *c = &msg->contents[i];

        free(c->text);
        free(c->tool_id);
        free(c->tool_name);

        if (c->tool_params) {
            cJSON_Delete(c->tool_params);
        }
        if (c->tool_output) {
            cJSON_Delete(c->tool_output);
        }
    }

    free(msg->contents);
    msg->contents = NULL;
    msg->content_count = 0;
}
