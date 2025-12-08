# Subagent Tool

## Overview

The Subagent tool allows claude-c to spawn a new instance of itself with a fresh context to work on delegated tasks. This is useful for:

1. **Context management** - Start with a clean slate without conversation history
2. **Task delegation** - Offload complex independent tasks to a separate agent
3. **Avoiding context limits** - Split large tasks across multiple contexts
4. **Parallel thinking** - Each subagent can explore different approaches

## Usage

```json
{
  "prompt": "Your task description here",
  "timeout": 300,
  "tail_lines": 100
}
```

**Parameters:**
- `prompt` (required, string) - The task description for the subagent
- `timeout` (optional, integer) - Timeout in seconds (default: 300, 0 = no timeout)
- `tail_lines` (optional, integer) - Number of lines to return from end of log (default: 100)

## How It Works

1. **Execution**: Spawns a new claude-c process with the given prompt
2. **Logging**: All stdout and stderr output is written to a timestamped log file in `.claude-c/subagent/`
3. **Return value**: Returns the tail of the log (last N lines) which typically contains the summary
4. **Full log access**: The complete log file path is provided for further inspection

## Output Structure

```json
{
  "exit_code": 0,
  "log_file": "/path/to/.claude-c/subagent/subagent_20231208_123456_1234.log",
  "total_lines": 42,
  "tail_lines_returned": 42,
  "tail_output": "Last N lines of subagent output...",
  "truncation_warning": "Optional warning if log was truncated"
}
```

## Best Practices

### When to Use Subagent

✅ **Good use cases:**
- Complex multi-step tasks that can be isolated
- Tasks requiring extensive file analysis (large codebases)
- When you're approaching context limits
- Delegating independent research or exploration
- Tasks that would benefit from a fresh perspective

❌ **Bad use cases:**
- Simple one-liner tasks (use direct tools instead)
- Tasks requiring shared conversation history
- When immediate real-time feedback is needed
- Highly interactive workflows

### Reading Subagent Output

The master agent should follow these guidelines:

1. **Start with the tail** - The returned `tail_output` typically contains the summary
2. **Check exit code** - `exit_code == 0` indicates success
3. **Count lines first** - Use `total_lines` to assess log size before reading
4. **Use Grep for search** - Search the log file for specific content rather than reading it all
5. **Read strategically** - Use Read tool with line ranges if you need specific sections

### Example Pattern

```
Master: "Use Subagent to analyze all Python files and create a report"
  ↓
Subagent: Spawned with fresh context
  - Uses Glob to find all *.py files
  - Uses Read to analyze each file
  - Uses Write to create report.txt
  - Returns summary in tail_output
  ↓
Master: Receives tail showing "Report created with 42 files analyzed"
  - Can Read the report.txt file if needed
  - Can Grep the log for errors or specific details
```

## Log File Management

**Location**: `.claude-c/subagent/subagent_YYYYMMDD_HHMMSS_PID.log`

**Format**: Timestamped filename with process ID for uniqueness

**Retention**: Log files accumulate over time. Consider periodic cleanup:
```bash
# Remove logs older than 7 days
find .claude-c/subagent/ -name "*.log" -mtime +7 -delete
```

## Limitations

1. **No streaming output** - The master agent waits for the subagent to complete
2. **No interrupt handling** - Once spawned, the subagent runs to completion or timeout
3. **No context sharing** - Subagent starts with clean context (this is by design)
4. **Resource usage** - Each subagent is a full claude-c instance with API calls

## Implementation Details

**Location**: `src/claude.c` - `tool_subagent()` function

**Key features:**
- Uses `system()` for process execution
- Proper shell escaping for quotes, backslashes, dollar signs, and backticks
- Redirects both stdout and stderr to log file
- Tail reading with configurable line count
- Returns log file path for further inspection

## Security Considerations

- The subagent inherits all environment variables (API keys, config)
- The subagent has full tool access (Write, Edit, Bash)
- Log files may contain sensitive information
- Shell escaping prevents command injection

## Troubleshooting

**Problem**: "Failed to read log file"
- Likely cause: Subagent failed to start or crashed immediately
- Check: Examine the exit code and error message
- Solution: Verify the prompt is valid and executable path is correct

**Problem**: "Subagent timed out"
- Exit code will be timeout-related
- Solution: Increase timeout parameter or simplify the task

**Problem**: "Output truncated"
- The tail_lines parameter limits returned content
- Solution: Use Read tool to access full log file, or increase tail_lines

**Problem**: "Task not completed"
- Check tail_output for errors or incomplete status
- Use Grep to search log for "error" or "failed"
- Read the full log file to understand what happened
