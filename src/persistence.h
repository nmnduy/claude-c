/*
 * Persistence Layer - SQLite3-based logging of API requests/responses
 *
 * This module provides functionality to persist all API interactions with the
 * inference backend (Anthropic/OpenAI) to a SQLite database for auditing,
 * debugging, and analysis purposes.
 */

#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <sqlite3.h>
#include <time.h>

// Database schema for api_calls table:
//
// CREATE TABLE IF NOT EXISTS api_calls (
//     id INTEGER PRIMARY KEY AUTOINCREMENT,
//     timestamp TEXT NOT NULL,           -- ISO 8601 format (YYYY-MM-DD HH:MM:SS)
//     session_id TEXT,                   -- Unique session identifier for grouping related API calls
//     api_base_url TEXT NOT NULL,        -- API endpoint URL (e.g., "https://api.openai.com/v1/chat/completions")
//     request_json TEXT NOT NULL,        -- Full JSON request sent to API
//     response_json TEXT,                -- Full JSON response received (NULL if error)
//     model TEXT NOT NULL,               -- Model name (e.g., "claude-sonnet-4-20250514")
//     status TEXT NOT NULL,              -- 'success' or 'error'
//     http_status INTEGER,               -- HTTP status code (200, 401, 500, etc.)
//     error_message TEXT,                -- Error message if status='error'
//     duration_ms INTEGER,               -- API call duration in milliseconds
//     tool_count INTEGER DEFAULT 0,      -- Number of tool_use blocks in response
//     created_at INTEGER NOT NULL        -- Unix timestamp for indexing/sorting
// );

// Persistence handle - opaque structure for database connection
typedef struct PersistenceDB {
    sqlite3 *db;
    char *db_path;
} PersistenceDB;

// Initialize persistence layer
// Opens/creates SQLite database and ensures schema is up to date
//
// Parameters:
//   db_path: Path to SQLite database file (NULL = use default location)
//            Default location priority:
//              1. $CLAUDE_C_DB_PATH (environment variable)
//              2. ./.claude-c/api_calls.db (project-local)
//              3. $XDG_DATA_HOME/claude-c/api_calls.db
//              4. ~/.local/share/claude-c/api_calls.db
//              5. ./api_calls.db (fallback)
//
// Returns:
//   PersistenceDB* on success, NULL on failure
PersistenceDB* persistence_init(const char *db_path);

// Log an API call to the database
//
// Parameters:
//   db: Persistence database handle
//   session_id: Unique session identifier (NULL if not available)
//   api_base_url: API endpoint URL (e.g., "https://api.openai.com/v1/chat/completions")
//   request_json: Raw JSON request string (must not be NULL)
//   response_json: Raw JSON response string (NULL if error occurred)
//   model: Model name used for the request
//   status: "success" or "error"
//   http_status: HTTP status code (0 if not available)
//   error_message: Error message (NULL if status="success")
//   duration_ms: Duration of API call in milliseconds
//   tool_count: Number of tool invocations in response (0 if none or error)
//
// Returns:
//   0 on success, -1 on failure
int persistence_log_api_call(
    PersistenceDB *db,
    const char *session_id,
    const char *api_base_url,
    const char *request_json,
    const char *response_json,
    const char *model,
    const char *status,
    int http_status,
    const char *error_message,
    long duration_ms,
    int tool_count
);

// Close persistence layer and free resources
void persistence_close(PersistenceDB *db);

// Get default database path
// Returns: Newly allocated string with default path (caller must free)
char* persistence_get_default_path(void);

#endif // PERSISTENCE_H
