/*
 * anthropic_provider.c - Anthropic Messages API provider implementation
 */

#define _POSIX_C_SOURCE 200809L

#include "claude_internal.h"  // Must be first to get ApiResponse definition
#include "anthropic_provider.h"
#include "openai_messages.h"  // We reuse internal message building and parse into OpenAI-like intermediate
#include "logger.h"
#include "http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <curl/curl.h>

#define DEFAULT_ANTHROPIC_URL "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION_HEADER "anthropic-version: 2023-06-01"

// ============================================================================
// CURL Helpers
// ============================================================================



static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;

    // clientp is the ConversationState* passed via progress_data parameter
    ConversationState *state = (ConversationState *)clientp;
    if (state && state->interrupt_requested) {
        LOG_DEBUG("Progress callback: interrupt requested, aborting HTTP request");
        return 1;  // Non-zero return aborts the curl transfer
    }

    return 0;
}

// Convert curl_slist headers to JSON string for logging
// Note: Using http_headers_to_json from http_client.h instead

// ============================================================================
// Anthropic Request/Response Conversion
// ============================================================================

// Convert OpenAI-style request (our internal builder outputs) to Anthropic native
static char* openai_to_anthropic_request(const char *openai_req) {
    cJSON *openai_json = cJSON_Parse(openai_req);
    if (!openai_json) return NULL;

    cJSON *messages = cJSON_GetObjectItem(openai_json, "messages");
    cJSON *tools = cJSON_GetObjectItem(openai_json, "tools");
    cJSON *max_tokens = cJSON_GetObjectItem(openai_json, "max_completion_tokens");
    cJSON *model = cJSON_GetObjectItem(openai_json, "model");

    cJSON *anth = cJSON_CreateObject();

    // Required fields
    if (model && cJSON_IsString(model)) {
        cJSON_AddStringToObject(anth, "model", model->valuestring);
    }
    cJSON_AddNumberToObject(anth, "max_tokens", (max_tokens && cJSON_IsNumber(max_tokens)) ? max_tokens->valueint : 8192);

    // Separate system and content messages
    cJSON *anth_msgs = cJSON_CreateArray();
    // Preserve system as content array if provided (to keep cache_control)
    cJSON *system_blocks = NULL;   // array of content blocks
    cJSON *system_string = NULL;   // fallback plain string

    if (messages && cJSON_IsArray(messages)) {
        cJSON *m = NULL;
        cJSON_ArrayForEach(m, messages) {
            cJSON *role = cJSON_GetObjectItem(m, "role");
            cJSON *content = cJSON_GetObjectItem(m, "content");
            if (!role || !cJSON_IsString(role)) continue;
            const char *r = role->valuestring;
            if (strcmp(r, "system") == 0) {
                if (cJSON_IsArray(content)) {
                    // Duplicate as-is to preserve any cache_control markers
                    system_blocks = cJSON_Duplicate(content, 1);
                } else if (cJSON_IsString(content)) {
                    system_string = cJSON_CreateString(content->valuestring);
                }
                continue;
            }

            cJSON *anth_m = cJSON_CreateObject();
            if (strcmp(r, "assistant") == 0) {
                cJSON_AddStringToObject(anth_m, "role", "assistant");
                cJSON *tool_calls = cJSON_GetObjectItem(m, "tool_calls");
                if (tool_calls && cJSON_IsArray(tool_calls)) {
                    cJSON *content_arr = cJSON_CreateArray();
                    if (cJSON_IsString(content) && content->valuestring && content->valuestring[0]) {
                        cJSON *tb = cJSON_CreateObject();
                        cJSON_AddStringToObject(tb, "type", "text");
                        cJSON_AddStringToObject(tb, "text", content->valuestring);
                        cJSON_AddItemToArray(content_arr, tb);
                    }
                    cJSON *tc = NULL;
                    cJSON_ArrayForEach(tc, tool_calls) {
                        cJSON *tb = cJSON_CreateObject();
                        cJSON_AddStringToObject(tb, "type", "tool_use");
                        cJSON *id = cJSON_GetObjectItem(tc, "id");
                        if (id && cJSON_IsString(id)) cJSON_AddStringToObject(tb, "id", id->valuestring);
                        cJSON *fn = cJSON_GetObjectItem(tc, "function");
                        if (fn) {
                            cJSON *name = cJSON_GetObjectItem(fn, "name");
                            cJSON *args = cJSON_GetObjectItem(fn, "arguments");
                            if (name && cJSON_IsString(name)) cJSON_AddStringToObject(tb, "name", name->valuestring);
                            if (args && cJSON_IsString(args)) {
                                cJSON *input = cJSON_Parse(args->valuestring);
                                if (!input) input = cJSON_CreateObject();
                                cJSON_AddItemToObject(tb, "input", input);
                            }
                        }
                        cJSON_AddItemToArray(content_arr, tb);
                    }
                    if (cJSON_GetArraySize(content_arr) > 0) {
                        cJSON_AddItemToObject(anth_m, "content", content_arr);
                    } else {
                        cJSON_Delete(content_arr);
                        cJSON_Delete(anth_m);
                        anth_m = NULL;
                    }
                } else {
                    if (cJSON_IsString(content) && content->valuestring && content->valuestring[0]) {
                        cJSON_AddStringToObject(anth_m, "content", content->valuestring);
                    } else {
                        cJSON_Delete(anth_m);
                        anth_m = NULL;
                    }
                }
            } else if (strcmp(r, "user") == 0) {
                cJSON_AddStringToObject(anth_m, "role", "user");
                if (cJSON_IsString(content)) {
                    cJSON_AddStringToObject(anth_m, "content", content->valuestring);
                } else if (cJSON_IsArray(content)) {
                    // Pass through content blocks (we already build image blocks for Bedrock)
                    cJSON_AddItemToObject(anth_m, "content", cJSON_Duplicate(content, 1));
                }
            } else if (strcmp(r, "tool") == 0) {
                cJSON *tool_call_id = cJSON_GetObjectItem(m, "tool_call_id");
                if (tool_call_id && cJSON_IsString(tool_call_id)) {
                    cJSON *content_arr = cJSON_CreateArray();
                    cJSON *tr = cJSON_CreateObject();
                    cJSON_AddStringToObject(tr, "type", "tool_result");
                    cJSON_AddStringToObject(tr, "tool_use_id", tool_call_id->valuestring);
                    if (cJSON_IsString(content)) {
                        cJSON_AddStringToObject(tr, "content", content->valuestring);
                    } else {
                        char *s = cJSON_PrintUnformatted(content);
                        cJSON_AddStringToObject(tr, "content", s ? s : "");
                        free(s);
                    }
                    cJSON_AddItemToArray(content_arr, tr);
                    cJSON_AddItemToObject(anth_m, "content", content_arr);
                }
                // Role remains user for tool results in Anthropic
                cJSON_AddStringToObject(anth_m, "role", "user");
            }

            if (anth_m && cJSON_GetObjectItem(anth_m, "role")) {
                cJSON_AddItemToArray(anth_msgs, anth_m);
            } else if (anth_m) {
                cJSON_Delete(anth_m);
            }
        }
    }

    cJSON_AddItemToObject(anth, "messages", anth_msgs);
    if (system_blocks) {
        // Anthropic supports system as either string or array of content blocks
        cJSON_AddItemToObject(anth, "system", system_blocks);
    } else if (system_string) {
        cJSON_AddItemToObject(anth, "system", system_string);
    }

    // Tools
    if (tools && cJSON_IsArray(tools)) {
        cJSON *anth_tools = cJSON_CreateArray();
        cJSON *t = NULL;
        cJSON_ArrayForEach(t, tools) {
            cJSON *fn = cJSON_GetObjectItem(t, "function");
            if (!fn) continue;
            cJSON *obj = cJSON_CreateObject();
            cJSON *name = cJSON_GetObjectItem(fn, "name");
            cJSON *desc = cJSON_GetObjectItem(fn, "description");
            cJSON *params = cJSON_GetObjectItem(fn, "parameters");
            if (name && cJSON_IsString(name)) cJSON_AddStringToObject(obj, "name", name->valuestring);
            if (desc && cJSON_IsString(desc)) cJSON_AddStringToObject(obj, "description", desc->valuestring);
            if (params) cJSON_AddItemToObject(obj, "input_schema", cJSON_Duplicate(params, 1));
            // Preserve cache_control on tool definitions to create checkpoint after tools
            cJSON *cache_ctrl = cJSON_GetObjectItem(t, "cache_control");
            if (cache_ctrl) {
                cJSON_AddItemToObject(obj, "cache_control", cJSON_Duplicate(cache_ctrl, 1));
            }
            cJSON_AddItemToArray(anth_tools, obj);
        }
        if (cJSON_GetArraySize(anth_tools) > 0) {
            cJSON_AddItemToObject(anth, "tools", anth_tools);
        } else {
            cJSON_Delete(anth_tools);
        }
    }

    // Version header is sent via HTTP header, not body. But some SDKs include anthropic_version
    const char *version_env = getenv("ANTHROPIC_VERSION");
    if (version_env && version_env[0]) {
        cJSON_AddStringToObject(anth, "anthropic_version", version_env);
    }

    char *out = cJSON_PrintUnformatted(anth);
    cJSON_Delete(openai_json);
    cJSON_Delete(anth);
    return out;
}

// Convert Anthropic JSON back to an OpenAI-like response so we can reuse parse code paths
static cJSON* anthropic_to_openai_response(const char *anthropic_raw) {
    cJSON *anth = cJSON_Parse(anthropic_raw);
    if (!anth) return NULL;

    cJSON *openai = cJSON_CreateObject();
    cJSON *choices = cJSON_CreateArray();
    cJSON *choice = cJSON_CreateObject();
    cJSON *message = cJSON_CreateObject();

    // Text content
    const char *text_out = NULL;
    cJSON *content = cJSON_GetObjectItem(anth, "content");
    if (cJSON_IsArray(content)) {
        // Find first text block
        cJSON *blk = NULL;
        cJSON_ArrayForEach(blk, content) {
            cJSON *type = cJSON_GetObjectItem(blk, "type");
            if (type && cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0) {
                cJSON *text = cJSON_GetObjectItem(blk, "text");
                if (text && cJSON_IsString(text)) {
                    text_out = text->valuestring;
                    break;
                }
            }
        }
    } else {
        cJSON *text = cJSON_GetObjectItem(anth, "content");
        if (cJSON_IsString(text)) text_out = text->valuestring;
    }

    if (text_out) cJSON_AddStringToObject(message, "content", text_out);
    else cJSON_AddNullToObject(message, "content");

    // Tool calls -> tool_calls array
    cJSON *tool_calls = NULL;
    if (cJSON_IsArray(content)) {
        cJSON *blk = NULL;
        cJSON_ArrayForEach(blk, content) {
            cJSON *type = cJSON_GetObjectItem(blk, "type");
            if (!type || !cJSON_IsString(type)) continue;
            if (strcmp(type->valuestring, "tool_use") == 0) {
                if (!tool_calls) tool_calls = cJSON_CreateArray();
                cJSON *tc = cJSON_CreateObject();
                cJSON_AddStringToObject(tc, "type", "function");
                cJSON *id = cJSON_GetObjectItem(blk, "id");
                if (id && cJSON_IsString(id)) cJSON_AddStringToObject(tc, "id", id->valuestring);
                cJSON *name = cJSON_GetObjectItem(blk, "name");
                cJSON *input = cJSON_GetObjectItem(blk, "input");
                cJSON *fn = cJSON_CreateObject();
                if (name && cJSON_IsString(name)) cJSON_AddStringToObject(fn, "name", name->valuestring);
                char *args_str = input ? cJSON_PrintUnformatted(input) : strdup("{}");
                if (args_str) {
                    cJSON_AddStringToObject(fn, "arguments", args_str);
                    free(args_str);
                }
                cJSON_AddItemToObject(tc, "function", fn);
                cJSON_AddItemToArray(tool_calls, tc);
            }
        }
    }

    if (tool_calls) cJSON_AddItemToObject(message, "tool_calls", tool_calls);

    cJSON_AddItemToObject(choice, "message", message);
    cJSON_AddItemToArray(choices, choice);
    cJSON_AddItemToObject(openai, "choices", choices);

    // Optional: usage -> convert if present
    cJSON *usage = cJSON_GetObjectItem(anth, "usage");
    if (usage) {
        cJSON *openai_usage = cJSON_CreateObject();
        cJSON *input_tokens = cJSON_GetObjectItem(usage, "input_tokens");
        cJSON *output_tokens = cJSON_GetObjectItem(usage, "output_tokens");
        if (cJSON_IsNumber(input_tokens)) cJSON_AddNumberToObject(openai_usage, "prompt_tokens", input_tokens->valuedouble);
        if (cJSON_IsNumber(output_tokens)) cJSON_AddNumberToObject(openai_usage, "completion_tokens", output_tokens->valuedouble);
        cJSON_AddItemToObject(openai, "usage", openai_usage);
    }

    cJSON_Delete(anth);
    return openai;
}

// ============================================================================
// Provider Implementation
// ============================================================================

static ApiCallResult anthropic_call_api(Provider *self, ConversationState *state) {
    ApiCallResult result = {0};
    AnthropicConfig *config = (AnthropicConfig*)self->config;

    if (!config || !config->api_key || !config->base_url) {
        result.error_message = strdup("Anthropic config or credentials not initialized");
        result.is_retryable = 0;
        return result;
    }

    // Build request JSON from internal messages (OpenAI-style), then convert
    int enable_caching = 1;  // Anthropic supports caching; ON by default unless disabled via env var
    const char *disable_env = getenv("DISABLE_PROMPT_CACHING");
    if (disable_env && (strcmp(disable_env, "1") == 0 || strcasecmp(disable_env, "true") == 0)) {
        enable_caching = 0;
    }

    cJSON *openai_req_obj = build_openai_request(state, enable_caching);
    if (!openai_req_obj) {
        result.error_message = strdup("Failed to build request JSON");
        result.is_retryable = 0;
        return result;
    }
    char *openai_req = cJSON_PrintUnformatted(openai_req_obj);
    cJSON_Delete(openai_req_obj);
    if (!openai_req) {
        result.error_message = strdup("Failed to serialize request JSON");
        result.is_retryable = 0;
        return result;
    }

    char *anth_req = openai_to_anthropic_request(openai_req);
    if (!anth_req) {
        result.error_message = strdup("Failed to convert request to Anthropic format");
        result.is_retryable = 0;
        result.request_json = openai_req;  // keep for logging
        return result;
    }

    // Set up headers
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // Authentication header: default x-api-key unless a custom template is provided
    char auth_header[512];
    if (config->auth_header_template) {
        const char *pct = strstr(config->auth_header_template, "%s");
        if (pct) {
            size_t pre = (size_t)(pct - config->auth_header_template);
            size_t keylen = strlen(config->api_key);
            size_t suf = strlen(pct + 2);
            if (pre + keylen + suf + 1 < sizeof(auth_header)) {
                strncpy(auth_header, config->auth_header_template, pre);
                auth_header[pre] = '\0';
                strncat(auth_header, config->api_key, sizeof(auth_header) - strlen(auth_header) - 1);
                strncat(auth_header, pct + 2, sizeof(auth_header) - strlen(auth_header) - 1);
            } else {
                strncpy(auth_header, config->auth_header_template, sizeof(auth_header) - 1);
                auth_header[sizeof(auth_header) - 1] = '\0';
                LOG_WARN("Auth header template too long, truncated");
            }
        } else {
            strncpy(auth_header, config->auth_header_template, sizeof(auth_header) - 1);
            auth_header[sizeof(auth_header) - 1] = '\0';
        }
    } else {
        snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", config->api_key);
    }
    headers = curl_slist_append(headers, auth_header);

    // Anthropic version header (allow override via OPENAI_EXTRA_HEADERS / env)
    const char *version_env = getenv("ANTHROPIC_VERSION");
    if (version_env && version_env[0]) {
        char buf[128];
        snprintf(buf, sizeof(buf), "anthropic-version: %s", version_env);
        headers = curl_slist_append(headers, buf);
    } else {
        headers = curl_slist_append(headers, ANTHROPIC_VERSION_HEADER);
    }

    // Extra headers from environment
    if (config->extra_headers) {
        for (int i = 0; i < config->extra_headers_count; i++) {
            if (config->extra_headers[i]) headers = curl_slist_append(headers, config->extra_headers[i]);
        }
    }

    if (!headers) {
        result.error_message = strdup("Failed to setup HTTP headers");
        result.is_retryable = 0;
        result.request_json = anth_req;
        result.headers_json = NULL;
        free(openai_req);
        return result;
    }

    // Build HTTP request using the new HTTP client
    HttpRequest req = {0};
    req.url = config->base_url;
    req.method = "POST";
    req.body = anth_req;
    req.headers = headers;
    req.connect_timeout_ms = 30000;  // 30 seconds
    req.total_timeout_ms = 300000;   // 5 minutes
    req.follow_redirects = 0;
    req.verbose = 0;

    // Execute HTTP request using the unified HTTP client
    HttpResponse *http_resp = http_client_execute(&req, progress_callback, state);

    // Convert headers to JSON for logging
    result.headers_json = http_headers_to_json(headers);

    // Free the headers list (http_client_execute makes its own copy)
    curl_slist_free_all(headers);

    // Keep request JSONs for logging
    result.request_json = anth_req;

    if (!http_resp) {
        result.error_message = strdup("Failed to execute HTTP request (memory allocation failed)");
        result.is_retryable = 0;
        free(result.headers_json);
        free(openai_req);
        return result;
    }

    result.duration_ms = http_resp->duration_ms;
    result.http_status = http_resp->status_code;

    // Handle HTTP errors
    if (http_resp->error_message) {
        result.error_message = strdup(http_resp->error_message);
        result.is_retryable = http_resp->is_retryable;
        http_response_free(http_resp);
        free(result.headers_json);
        free(openai_req);
        return result;
    }

    result.raw_response = http_resp->body ? strdup(http_resp->body) : NULL;
    http_response_free(http_resp);

    if (result.http_status >= 200 && result.http_status < 300) {
        // Convert to OpenAI-like then parse as in other providers
        cJSON *openai_like = anthropic_to_openai_response(result.raw_response);
        if (!openai_like) {
            result.error_message = strdup("Failed to parse Anthropic response");
            result.is_retryable = 0;
            free(result.headers_json);
            free(openai_req);
            return result;
        }

        ApiResponse *api_resp = calloc(1, sizeof(ApiResponse));
        if (!api_resp) {
            result.error_message = strdup("Failed to allocate ApiResponse");
            result.is_retryable = 0;
            cJSON_Delete(openai_like);
            free(result.headers_json);
            free(openai_req);
            return result;
        }

        api_resp->raw_response = openai_like;

        cJSON *choices = cJSON_GetObjectItem(openai_like, "choices");
        if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
            result.error_message = strdup("Invalid response format: no choices");
            result.is_retryable = 0;
            api_response_free(api_resp);
            free(result.headers_json);
            free(openai_req);
            return result;
        }
        cJSON *choice = cJSON_GetArrayItem(choices, 0);
        cJSON *message = cJSON_GetObjectItem(choice, "message");
        if (!message) {
            result.error_message = strdup("Invalid response format: no message");
            result.is_retryable = 0;
            api_response_free(api_resp);
            free(result.headers_json);
            free(openai_req);
            return result;
        }

        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && cJSON_IsString(content) && content->valuestring) {
            api_resp->message.text = strdup(content->valuestring);
        }

        cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        if (tool_calls && cJSON_IsArray(tool_calls)) {
            int raw_count = cJSON_GetArraySize(tool_calls);
            int valid = 0;
            for (int i = 0; i < raw_count; i++) {
                cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
                if (cJSON_GetObjectItem(tc, "function")) valid++;
            }
            if (valid > 0) {
                api_resp->tools = calloc((size_t)valid, sizeof(ToolCall));
                if (!api_resp->tools) {
                    result.error_message = strdup("Failed to allocate tool calls");
                    result.is_retryable = 0;
                    api_response_free(api_resp);
                    free(result.headers_json);
                    free(openai_req);
                    return result;
                }
                int idx = 0;
                for (int i = 0; i < raw_count; i++) {
                    cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
                    cJSON *id = cJSON_GetObjectItem(tc, "id");
                    cJSON *fn = cJSON_GetObjectItem(tc, "function");
                    if (!fn) continue;
                    cJSON *name = cJSON_GetObjectItem(fn, "name");
                    cJSON *args = cJSON_GetObjectItem(fn, "arguments");
                    api_resp->tools[idx].id = (id && cJSON_IsString(id)) ? strdup(id->valuestring) : NULL;
                    api_resp->tools[idx].name = (name && cJSON_IsString(name)) ? strdup(name->valuestring) : NULL;
                    if (args && cJSON_IsString(args)) {
                        api_resp->tools[idx].parameters = cJSON_Parse(args->valuestring);
                        if (!api_resp->tools[idx].parameters) api_resp->tools[idx].parameters = cJSON_CreateObject();
                    } else {
                        api_resp->tools[idx].parameters = cJSON_CreateObject();
                    }
                    idx++;
                }
                api_resp->tool_count = valid;
            }
        }

        result.response = api_resp;
        free(openai_req);
        return result;
    }

    // HTTP error handling
    result.is_retryable = (result.http_status == 429 || result.http_status == 408 || result.http_status >= 500);

    // Try to extract message
    cJSON *err = cJSON_Parse(result.raw_response);
    if (err) {
        // Anthropic error shape has error.message
        cJSON *error_obj = cJSON_GetObjectItem(err, "error");
        if (error_obj) {
            cJSON *msg = cJSON_GetObjectItem(error_obj, "message");
            cJSON *type = cJSON_GetObjectItem(error_obj, "type");
            if (msg && cJSON_IsString(msg)) {
                const char *txt = msg->valuestring;
                const char *type_txt = (type && cJSON_IsString(type)) ? type->valuestring : "";
                if (strstr(txt, "maximum context length") || strstr(txt, "too many tokens") || (strcmp(type_txt, "invalid_request_error") == 0 && strstr(txt, "tokens"))) {
                    result.error_message = strdup("Context length exceeded. The conversation has grown too large for the model's memory. Try starting a new conversation or reduce the amount of code/files being discussed.");
                    result.is_retryable = 0;
                } else {
                    result.error_message = strdup(txt);
                }
            }
        }
        cJSON_Delete(err);
    }

    if (!result.error_message) {
        char buf[256];
        snprintf(buf, sizeof(buf), "HTTP %ld", result.http_status);
        result.error_message = strdup(buf);
    }

    free(result.headers_json);
    free(openai_req);
    return result;
}

static void anthropic_cleanup(Provider *self) {
    if (!self) return;
    if (self->config) {
        AnthropicConfig *cfg = (AnthropicConfig*)self->config;
        free(cfg->api_key);
        free(cfg->base_url);
        free(cfg->auth_header_template);
        if (cfg->extra_headers) {
            for (int i = 0; i < cfg->extra_headers_count; i++) free(cfg->extra_headers[i]);
            free(cfg->extra_headers);
        }
        free(cfg);
    }
    free(self);
}

Provider* anthropic_provider_create(const char *api_key, const char *base_url) {
    LOG_DEBUG("Creating Anthropic provider...");
    if (!api_key || !api_key[0]) {
        LOG_ERROR("Anthropic provider: API key is required");
        return NULL;
    }

    Provider *p = calloc(1, sizeof(Provider));
    if (!p) return NULL;
    AnthropicConfig *cfg = calloc(1, sizeof(AnthropicConfig));
    if (!cfg) { free(p); return NULL; }

    cfg->api_key = strdup(api_key);
    if (!cfg->api_key) { free(cfg); free(p); return NULL; }

    if (base_url && base_url[0]) cfg->base_url = strdup(base_url);
    else cfg->base_url = strdup(DEFAULT_ANTHROPIC_URL);
    if (!cfg->base_url) { free(cfg->api_key); free(cfg); free(p); return NULL; }

    // Auth header template: prefer OPENAI_AUTH_HEADER if set, else default to x-api-key
    const char *auth_env = getenv("OPENAI_AUTH_HEADER");
    if (auth_env && auth_env[0]) {
        cfg->auth_header_template = strdup(auth_env);
        if (!cfg->auth_header_template) { free(cfg->base_url); free(cfg->api_key); free(cfg); free(p); return NULL; }
    }

    // Extra headers
    const char *extra_env = getenv("OPENAI_EXTRA_HEADERS");
    if (extra_env && extra_env[0]) {
        char *copy = strdup(extra_env);
        if (!copy) { free(cfg->auth_header_template); free(cfg->base_url); free(cfg->api_key); free(cfg); free(p); return NULL; }
        int count = 0; char *tok = strtok(copy, ",");
        while (tok) { count++; tok = strtok(NULL, ","); }
        cfg->extra_headers = calloc((size_t)count + 1, sizeof(char*));
        if (!cfg->extra_headers) { free(copy); free(cfg->auth_header_template); free(cfg->base_url); free(cfg->api_key); free(cfg); free(p); return NULL; }
        strcpy(copy, extra_env);
        tok = strtok(copy, ",");
        for (int i = 0; i < count && tok; i++) {
            while (*tok == ' ' || *tok == '\t') tok++;
            char *end = tok + strlen(tok) - 1;
            while (end > tok && (*end == ' ' || *end == '\t')) end--;
            *(end + 1) = '\0';
            cfg->extra_headers[i] = strdup(tok);
            if (!cfg->extra_headers[i]) {
                for (int j = 0; j < i; j++) free(cfg->extra_headers[j]);
                free(cfg->extra_headers); free(copy); free(cfg->auth_header_template); free(cfg->base_url); free(cfg->api_key); free(cfg); free(p); return NULL; }
            tok = strtok(NULL, ",");
        }
        cfg->extra_headers_count = count;
        cfg->extra_headers[count] = NULL;
        free(copy);
    }

    p->name = "Anthropic";
    p->config = cfg;
    p->call_api = anthropic_call_api;
    p->cleanup = anthropic_cleanup;

    LOG_INFO("Anthropic provider created (endpoint: %s)", cfg->base_url);
    return p;
}
