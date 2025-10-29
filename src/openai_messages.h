/*
 * openai_messages.h - OpenAI message format conversion
 *
 * Converts between internal vendor-agnostic message format
 * and OpenAI's API message format (with tool_calls)
 */

#ifndef OPENAI_MESSAGES_H
#define OPENAI_MESSAGES_H

#include <cjson/cJSON.h>
#include "claude_internal.h"

/**
 * Build OpenAI request JSON from internal message format
 *
 * Converts InternalMessage[] to OpenAI's message format:
 * - Assistant messages: { role: "assistant", content: "...", tool_calls: [...] }
 * - User messages: { role: "user", content: "..." }
 * - Tool responses: { role: "tool", tool_call_id: "...", content: "..." }
 *
 * @param state - Conversation state with internal messages
 * @param enable_caching - Whether to add Anthropic cache_control markers (for compatible APIs)
 * @return JSON object with OpenAI request (caller must free), or NULL on error
 */
cJSON* build_openai_request(ConversationState *state, int enable_caching);

/**
 * Parse OpenAI response into internal message format
 *
 * Converts OpenAI API response to InternalMessage:
 * - content: "..." -> INTERNAL_TEXT
 * - tool_calls: [...] -> INTERNAL_TOOL_CALL blocks
 *
 * @param response - OpenAI API response JSON
 * @return InternalMessage (caller must free contents), or empty message on error
 */
InternalMessage parse_openai_response(cJSON *response);

/**
 * Free internal message contents
 *
 * @param msg - Message to free (struct itself is not freed)
 */
void free_internal_message(InternalMessage *msg);

#endif // OPENAI_MESSAGES_H
