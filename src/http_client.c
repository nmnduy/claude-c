/*
 * http_client.c - Unified HTTP client implementation
 */

#define _POSIX_C_SOURCE 200809L

#include "http_client.h"
#include "logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

// ============================================================================
// Internal Types
// ============================================================================

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} MemoryBuffer;

typedef struct {
    MemoryBuffer *buffer;
    struct curl_slist *headers;
} WriteContext;

// ============================================================================
// Static Helpers
// ============================================================================

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    WriteContext *ctx = (WriteContext *)userp;
    MemoryBuffer *buf = ctx->buffer;

    // Ensure we have enough capacity
    size_t needed = buf->size + realsize + 1;
    if (needed > buf->capacity) {
        // Grow by at least 2x or to needed size
        size_t new_capacity = buf->capacity * 2;
        if (new_capacity < needed) {
            new_capacity = needed;
        }
        
        char *new_data = realloc(buf->data, new_capacity);
        if (!new_data) {
            LOG_ERROR("Failed to allocate memory for HTTP response (needed: %zu)", needed);
            return 0;
        }
        
        buf->data = new_data;
        buf->capacity = new_capacity;
    }

    // Copy data
    memcpy(buf->data + buf->size, contents, realsize);
    buf->size += realsize;
    buf->data[buf->size] = '\0';

    return realsize;
}

static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t realsize = size * nitems;
    WriteContext *ctx = (WriteContext *)userdata;
    
    // Null-terminate the header line
    char *header = malloc(realsize + 1);
    if (!header) {
        LOG_ERROR("Failed to allocate memory for header");
        return 0;
    }
    
    memcpy(header, buffer, realsize);
    header[realsize] = '\0';
    
    // Remove trailing CRLF
    if (realsize >= 2 && header[realsize-2] == '\r' && header[realsize-1] == '\n') {
        header[realsize-2] = '\0';
    } else if (realsize >= 1 && header[realsize-1] == '\n') {
        header[realsize-1] = '\0';
    }
    
    // Add to headers list
    ctx->headers = curl_slist_append(ctx->headers, header);
    free(header);
    
    return realsize;
}

static MemoryBuffer* memory_buffer_create(void) {
    MemoryBuffer *buf = calloc(1, sizeof(MemoryBuffer));
    if (!buf) {
        return NULL;
    }
    
    // Initial capacity
    buf->capacity = 4096;
    buf->data = malloc(buf->capacity);
    if (!buf->data) {
        free(buf);
        return NULL;
    }
    
    buf->data[0] = '\0';
    return buf;
}

static void memory_buffer_free(MemoryBuffer *buf) {
    if (buf) {
        free(buf->data);
        free(buf);
    }
}



// ============================================================================
// Public Functions
// ============================================================================

int http_client_init(void) {
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        LOG_ERROR("Failed to initialize libcurl: %s", curl_easy_strerror(res));
        return -1;
    }
    return 0;
}

void http_client_cleanup(void) {
    curl_global_cleanup();
}

HttpResponse* http_client_execute(const HttpRequest *req,
                                 HttpProgressCallback progress_cb,
                                 void *progress_data) {
    if (!req || !req->url) {
        LOG_ERROR("Invalid HTTP request: NULL request or URL");
        return NULL;
    }
    
    HttpResponse *resp = calloc(1, sizeof(HttpResponse));
    if (!resp) {
        LOG_ERROR("Failed to allocate HTTP response");
        return NULL;
    }
    
    CURL *curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to initialize CURL handle");
        free(resp);
        return NULL;
    }
    
    // Initialize buffers
    MemoryBuffer *body_buf = memory_buffer_create();
    WriteContext write_ctx = {0};
    write_ctx.buffer = body_buf;
    
    if (!body_buf) {
        LOG_ERROR("Failed to allocate memory buffer");
        curl_easy_cleanup(curl);
        free(resp);
        return NULL;
    }
    
    // Set up curl options
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    
    // Method
    if (req->method && strcmp(req->method, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (req->method && strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (req->body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
        }
    } else if (req->method && strcmp(req->method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (req->body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
        }
    } else {
        // Default to POST
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        if (req->body) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
        }
    }
    
    // Headers
    if (req->headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, req->headers);
    }
    
    // Timeouts (convert ms to seconds for curl)
    if (req->connect_timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, req->connect_timeout_ms);
    } else {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L); // Default 30 seconds
    }
    
    if (req->total_timeout_ms > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, req->total_timeout_ms);
    } else {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L); // Default 5 minutes
    }
    
    // Redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req->follow_redirects ? 1L : 0L);
    
    // Verbose logging
    curl_easy_setopt(curl, CURLOPT_VERBOSE, req->verbose ? 1L : 0L);
    
    // Callbacks
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_ctx);
    
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &write_ctx);
    
    // Progress callback
    if (progress_cb) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, progress_data);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }
    
    // Execute request
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    CURLcode res = curl_easy_perform(curl);
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    resp->duration_ms = (end.tv_sec - start.tv_sec) * 1000 +
                       (end.tv_nsec - start.tv_nsec) / 1000000;
    
    // Get status code
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status_code);
    
    // Handle result
    if (res != CURLE_OK) {
        // Check if the error was due to user interruption
        if (res == CURLE_ABORTED_BY_CALLBACK) {
            resp->error_message = strdup("Request interrupted by user");
            resp->is_retryable = 0;
        } else {
            resp->error_message = strdup(curl_easy_strerror(res));
            resp->is_retryable = (res == CURLE_COULDNT_CONNECT ||
                                 res == CURLE_OPERATION_TIMEDOUT ||
                                 res == CURLE_RECV_ERROR ||
                                 res == CURLE_SEND_ERROR ||
                                 res == CURLE_SSL_CONNECT_ERROR ||
                                 res == CURLE_GOT_NOTHING);
        }
        
        // Clean up buffers on error
        memory_buffer_free(body_buf);
        curl_slist_free_all(write_ctx.headers);
    } else {
        // Success - transfer ownership
        resp->body = body_buf->data;
        body_buf->data = NULL; // Prevent double free
        resp->headers = write_ctx.headers;
        
        // Body buffer is now owned by HttpResponse
        free(body_buf);
    }
    
    curl_easy_cleanup(curl);
    return resp;
}

void http_response_free(HttpResponse *resp) {
    if (!resp) return;
    
    free(resp->body);
    curl_slist_free_all(resp->headers);
    free(resp->error_message);
    free(resp);
}

char* http_headers_to_json(struct curl_slist *headers) {
    if (!headers) {
        return NULL;
    }
    
    cJSON *headers_array = cJSON_CreateArray();
    if (!headers_array) {
        return NULL;
    }
    
    struct curl_slist *current = headers;
    while (current) {
        if (current->data) {
            cJSON *header_obj = cJSON_CreateObject();
            if (header_obj) {
                // Parse header line into name and value
                char *colon = strchr(current->data, ':');
                if (colon) {
                    *colon = '\0';  // Split the string
                    char *header_name = current->data;
                    char *header_value = colon + 1;
                    
                    // Skip leading whitespace in value
                    while (*header_value == ' ' || *header_value == '\t') {
                        header_value++;
                    }
                    
                    cJSON_AddStringToObject(header_obj, "name", header_name);
                    cJSON_AddStringToObject(header_obj, "value", header_value);
                    
                    *colon = ':';  // Restore the colon
                } else {
                    // If no colon, treat the whole line as a header line
                    cJSON_AddStringToObject(header_obj, "line", current->data);
                }
                cJSON_AddItemToArray(headers_array, header_obj);
            }
        }
        current = current->next;
    }
    
    char *json_str = cJSON_PrintUnformatted(headers_array);
    cJSON_Delete(headers_array);
    return json_str;
}

struct curl_slist* http_copy_headers(struct curl_slist *headers) {
    struct curl_slist *new_list = NULL;
    struct curl_slist *current = headers;
    
    while (current) {
        if (current->data) {
            new_list = curl_slist_append(new_list, current->data);
            if (!new_list) {
                // Failed to allocate, clean up what we've created
                curl_slist_free_all(new_list);
                return NULL;
            }
        }
        current = current->next;
    }
    
    return new_list;
}

struct curl_slist* http_add_header(struct curl_slist *headers, const char *header) {
    return curl_slist_append(headers, header);
}
