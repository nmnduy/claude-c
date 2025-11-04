/*
 * ai_worker.c - Background worker for asynchronous API requests
 */

#include "ai_worker.h"
#include "logger.h"
#include <stdlib.h>

static void* ai_worker_thread_main(void *arg) {
    AIWorkerContext *ctx = (AIWorkerContext *)arg;
    if (!ctx) {
        return NULL;
    }

    while (ctx->running) {
        AIInstruction instruction = {0};
        int rc = dequeue_instruction(ctx->instruction_queue, &instruction);
        if (rc == 0) {
            /* Queue shutdown */
            break;
        }
        if (rc < 0) {
            LOG_ERROR("AI worker failed to dequeue instruction");
            continue;
        }

        if (!ctx->running) {
            free(instruction.text);
            break;
        }

        if (ctx->handler) {
            ctx->handler(ctx, &instruction);
        }

        free(instruction.text);
    }

    return NULL;
}

int ai_worker_start(AIWorkerContext *ctx,
                    ConversationState *state,
                    AIInstructionQueue *instruction_queue,
                    TUIMessageQueue *tui_queue,
                    AIWorkerHandler handler) {
    if (!ctx || !instruction_queue || !tui_queue || !state || !handler) {
        return -1;
    }

    ctx->instruction_queue = instruction_queue;
    ctx->tui_queue = tui_queue;
    ctx->state = state;
    ctx->handler = handler;
    ctx->running = 1;
    ctx->thread_started = 0;

    int rc = pthread_create(&ctx->thread, NULL, ai_worker_thread_main, ctx);
    if (rc != 0) {
        LOG_ERROR("Failed to create AI worker thread (rc=%d)", rc);
        ctx->running = 0;
        return -1;
    }

    ctx->thread_started = 1;
    return 0;
}

void ai_worker_stop(AIWorkerContext *ctx) {
    if (!ctx) {
        return;
    }

    if (!ctx->thread_started) {
        return;
    }

    ctx->running = 0;
    ai_queue_shutdown(ctx->instruction_queue);
    pthread_join(ctx->thread, NULL);
    ctx->thread_started = 0;
}

int ai_worker_submit(AIWorkerContext *ctx, const char *text) {
    if (!ctx || !text || !ctx->instruction_queue) {
        return -1;
    }
    return enqueue_instruction(ctx->instruction_queue, text, ctx->state);
}
