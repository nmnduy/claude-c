# API Call Persistence Layer

The Claude C implementation includes a SQLite3-based persistence layer that automatically logs all API requests and responses to a local database. This provides audit trails, debugging capabilities, and usage analytics.

## Features

- **Automatic logging**: Every API call is logged with full request/response data
- **Performance tracking**: Records duration and HTTP status for each call
- **Tool usage tracking**: Counts tool invocations per API call
- **Error logging**: Captures errors with full details
- **Configurable location**: Database path can be customized via environment variable
- **Query tool**: Built-in utility for inspecting logged data

## Database Schema

The `api_calls` table stores:

| Field | Type | Description |
|-------|------|-------------|
| `id` | INTEGER | Auto-incrementing primary key |
| `timestamp` | TEXT | Human-readable timestamp (ISO 8601) |
| `api_base_url` | TEXT | API endpoint URL used for the request |
| `request_json` | TEXT | Full JSON request sent to API |
| `response_json` | TEXT | Full JSON response received (NULL if error) |
| `model` | TEXT | Model name (e.g., "o4-mini") |
| `status` | TEXT | "success" or "error" |
| `http_status` | INTEGER | HTTP status code (200, 401, 500, etc.) |
| `error_message` | TEXT | Error message if status='error' |
| `duration_ms` | INTEGER | API call duration in milliseconds |
| `tool_count` | INTEGER | Number of tool_use blocks in response |
| `created_at` | INTEGER | Unix timestamp for indexing/sorting |

## Configuration

### Database Location

The database path is determined in this priority order:

1. **`CLAUDE_DB_PATH`** environment variable
2. **`$XDG_DATA_HOME/claude/api_calls.db`** (typically `~/.local/share/claude/api_calls.db`)
3. **`~/.local/share/claude/api_calls.db`** (fallback if XDG not set)
4. **`./api_calls.db`** (current directory, last resort)

Example:
```bash
export CLAUDE_DB_PATH="/path/to/my/logs.db"
./build/claude
```

### Disabling Persistence

Persistence is enabled by default. If initialization fails, a warning is logged but the application continues without persistence.

## Query Tool

A command-line utility is provided to query and analyze logged API calls.

### Building the Query Tool

```bash
make query-tool
```

This creates `./build/query_logs`.

### Usage Examples

**Show last 10 API calls** (default):
```bash
./build/query_logs
```

**Show all API calls**:
```bash
./build/query_logs --all
```

**Show only errors**:
```bash
./build/query_logs --errors
```

**Show statistics**:
```bash
./build/query_logs --stats
```
Output includes:
- Total API calls (success/failure counts)
- Duration statistics (min/max/average)
- Total tool invocations
- Models used

**Use custom database**:
```bash
./build/query_logs --db /path/to/custom.db --stats
```

### Example Output

```
Database: /Users/username/.local/share/claude/api_calls.db

[ID: 5] 2025-01-15 10:23:45
  Provider: https://api.openai.com/v1/chat/completions
  Model: o4-mini
  Status: success (HTTP 200)
  Duration: 1523 ms
  Tools: 2

[ID: 4] 2025-01-15 10:22:10
  Provider: https://api.anthropic.com/v1/messages
  Model: o4-mini
  Status: error (HTTP 401) - Invalid API key
  Duration: 234 ms
  Tools: 0

Total: 2 calls
```

**Statistics Example**:
```
=== API Call Statistics ===
Total API calls: 42
  Successful: 38 (90.5%)
  Failed: 4 (9.5%)

Duration:
  Average: 1456.2 ms
  Min: 234 ms
  Max: 3421 ms

Total tool invocations: 87
Average tools per call: 2.07

=== Models Used ===
  o4-mini: 42 calls
```

## Direct Database Access

Since the persistence layer uses SQLite3, you can query the database directly using the `sqlite3` CLI:

```bash
# Open database
sqlite3 ~/.local/share/claude/api_calls.db

# Example queries
.mode column
.headers on

-- Show recent calls
SELECT id, timestamp, api_base_url, model, status, duration_ms, tool_count
FROM api_calls
ORDER BY created_at DESC
LIMIT 10;

-- Show error rate by provider
SELECT
  api_base_url,
  COUNT(*) as total,
  SUM(CASE WHEN status='error' THEN 1 ELSE 0 END) as errors,
  ROUND(100.0 * SUM(CASE WHEN status='error' THEN 1 ELSE 0 END) / COUNT(*), 2) as error_rate
FROM api_calls
GROUP BY api_base_url;

-- Show slowest calls
SELECT timestamp, api_base_url, model, duration_ms, tool_count
FROM api_calls
WHERE status='success'
ORDER BY duration_ms DESC
LIMIT 5;

-- Export request/response for a specific call
SELECT request_json, response_json
FROM api_calls
WHERE id = 42;
```

## Implementation Details

### Code Location

- **Header**: `src/persistence.h` - Public API and documentation
- **Implementation**: `src/persistence.c` - SQLite operations
- **Integration**: `src/claude.c` (lines ~950-1073) - Logging in `call_api()`
- **Query Tool**: `tools/query_logs.c` - Standalone query utility

### Key Functions

```c
// Initialize persistence (called in main)
PersistenceDB* persistence_init(const char *db_path);

// Log an API call (called after each API request)
int persistence_log_api_call(
    PersistenceDB *db,
    const char *request_json,
    const char *response_json,
    const char *model,
    const char *status,
    int http_status,
    const char *error_message,
    long duration_ms,
    int tool_count
);

// Close persistence (called during cleanup)
void persistence_close(PersistenceDB *db);
```

### Performance Considerations

- **Non-blocking**: SQLite operations use WAL mode for better concurrency
- **Indexed**: The `timestamp` column is indexed for fast queries
- **Minimal overhead**: Logging adds ~1-5ms per API call
- **Automatic cleanup**: Database is properly closed on exit

### Error Handling

- Database initialization failures are logged but don't crash the application
- Failed log writes are reported to stderr but don't interrupt operation
- Corrupted databases can be deleted to reset

## Privacy and Security

**⚠️ Important Considerations:**

- The database contains **full API request/response data**, including:
  - Your prompts and the AI's responses
  - Tool invocations and results (file contents, command outputs, etc.)
  - API keys are **NOT** stored (excluded from logged data)

- **Protect the database file**: Treat it as sensitive data
  - Set appropriate file permissions: `chmod 600 ~/.local/share/claude/api_calls.db`
  - Exclude from version control and backups if needed
  - Consider encrypting the filesystem containing the database

- **Disk usage**: The database can grow large over time
  - Monitor disk usage periodically
  - Manually delete old entries or the entire database if needed
  - Future enhancement: Add automatic retention policies

## Schema Updates

If you're upgrading from an older version that doesn't include the `api_base_url` field, you'll need to either:

**Option 1: Migrate the existing database**
```bash
sqlite3 ~/.local/share/claude/api_calls.db

-- Add the new column (existing records will have NULL)
ALTER TABLE api_calls ADD COLUMN api_base_url TEXT;

-- Optionally, set a default value for existing records
UPDATE api_calls SET api_base_url = 'https://api.openai.com/v1/chat/completions' WHERE api_base_url IS NULL;
```

**Option 2: Start fresh** (easiest, but loses history)
```bash
# Backup old database (optional)
mv ~/.local/share/claude/api_calls.db ~/.local/share/claude/api_calls.db.old

# Next run will create a new database with updated schema
```

## Troubleshooting

**Database initialization fails**:
```bash
# Check if directory exists and is writable
mkdir -p ~/.local/share/claude
chmod 755 ~/.local/share/claude

# Try with explicit path
export CLAUDE_DB_PATH="/tmp/claude_test.db"
./build/claude
```

**Query tool shows "No API calls found"**:
- Ensure you've made at least one API call with claude
- Check if using correct database path
- Verify database file exists: `ls -l ~/.local/share/claude/api_calls.db`

**Database is corrupted**:
```bash
# Backup and reset
mv ~/.local/share/claude/api_calls.db ~/.local/share/claude/api_calls.db.backup
# Next claude run will create fresh database
```

## Future Enhancements

Potential improvements for the persistence layer:

- [ ] Automatic database cleanup/retention policies (e.g., delete entries older than 30 days)
- [ ] Export to JSON/CSV for analysis in other tools
- [ ] Compression of old entries to save disk space
- [ ] Web-based query interface
- [ ] Integration with observability platforms (e.g., export to Prometheus/Grafana)
- [ ] Encryption at rest for sensitive data
- [ ] Session grouping (track related API calls in a conversation)
