- [ ] fix release.yaml
- [x] why so many linse between [Assistant] and everything else?

- [ ] plan mode
- [ ] remove ANTHROPIC_MODEL env var usage. its confusing
- [ ] colorize the diff using the colorscheme we got
- [x] tmux paste doesn't get handled correctly in the input
- [ ] object files in same dir?
- [ ] receive user input
- [ ] write tool should also show diff
- [x] print todo list after creating it or when updating the TODO list
- [ ] {"message":"Too many tokens, please wait before trying again."}. if api responded with this then handle appropriately
- [ ] thinking vs non thinking
- [ ] fast model vs slow model
- [x] write state and logs to ./.claude-c by default
- [x] ctrl + l to clear input box
- [x] env var for custom log dir and api call db file
- [ ] rotation for api call db files
- [x] pasting still fucks up the terminal
- [ ] add retries to falied API calls
- [ ] implement instruction queue. so user can keep typing

- [ ]
```sh
[2025-10-28 02:18:55] [sess_1761635777_511588dc] WARN  [indicators.h:53] get_spinner_color_tool: Using fallback color for spinner (tool)
```

- [x] why does it fail with failed to resolve path. isn't that path same dir? log debug more when we see this to help fixing the problem. maybe unit test this tool more.
```sh
[Tool: Write] test_todo_simple.c
[Error] Write failed: Failed to resolve path
[Assistant] Let me check the current directory:
```

- [x] can we use ctrl + J for new line. what trick this node js claude used to achieve this?

- [ ]
Goodbye!
claude-c(35580,0x209b9a0c0) malloc: *** error for object 0x6f: pointer being freed was not allocated
claude-c(35580,0x209b9a0c0) malloc: *** set a breakpoint in malloc_error_break to debug
Abort trap: 6

- [x]
[Tool: Write] claude_todo_list_todo.md
[Error] Write failed: Failed to resolve path
[Tool: Write] claude_todo_list_todo.md
[Error] Write failed: Failed to resolve path
[Tool: Bash] pwd
[Tool: Write] claude_todo_list_todo.md
[Error] Write failed: Failed to resolve path
[Tool: Bash] ls -la

- [x]
```sh
[Tool: TodoWrite] (null)
⠸ Processing tool results...
```

- [ ] pasting still causes enter
- [x] large paste fails. how does node js claude prevents that and show X lines pasted.
- [x] pressing Esc will wait for tool to finish but there is no indicator
    - [ ] 'esc' doesn't show interrupted right away.
    - [ ] 'esc' should interrupt certain tools right away and dont wait
- [x] logs are not flushed often?
- [x] ctrl + c twice to exit. dont exit right away on ctrl + c
- [x] all log records should have session id tag. sessions are essential and each run has a new session id

- [ ] rust build messes up the terminal
```sh
⠏ Running 1 tool...   Compiling spl-associated-token-account-client v2.0.0
⠸ Running 1 tool...   Compiling solana-compute-budget-interface v2.2.2
⠼ Running 1 tool...   Compiling solana-rent-debits v2.2.1
⠧ Running 1 tool...   Compiling solana-epoch-rewards-hasher v2.2.1
⠏ Running 1 tool...    Building [=======================> ] 732/762: solana-streamer, solan
⠙ Running 1 tool...   Compiling solana-sdk v2.3.1
⠹ Running 1 tool...   Compiling solana-quic-client v2.3.13
⠸ Running 1 tool...   Compiling solana-udp-client v2.3.13
⠦ Running 1 tool...   Compiling solana-account-decoder v2.3.13
⠇ Running 1 tool...   Compiling spl-associated-token-account v6.0.0
⠦ Running 1 tool...    Building [=======================> ] 738/762: solana-streamer, solan
⠋ Running 1 tool...   Compiling pyth-sdk-solana v0.10.6
⠸ Running 1 tool...   Compiling spl-token v6.0.0
⠇ Running 1 tool...   Compiling solana-rpc-client-nonce-utils v2.3.13
⠋ Running 1 tool...   Compiling solana-thin-client v2.3.13
⠙ Running 1 tool...   Compiling solana-tpu-client v2.3.13
⠦ Running 1 tool...   Compiling uniffi_core v0.28.3
⠇ Running 1 tool...   Compiling bip39 v2.2.0
⠋ Running 1 tool...   Compiling rusqlite v0.32.1
⠙ Running 1 tool...    Building [=======================> ] 748/762: bip39, solana-rpc-clie
   Compiling solana-client v2.3.13
⠼ Running 1 tool...   Compiling uniffi v0.28.3
```

- [ ] we need this output
```sh
⏺ Update(src/aws_bedrock.c)
  ⎿  Updated src/aws_bedrock.c with 18 additions and 4 removals
       338            creds->region = strdup(region ? region : "us-west-2");
       339            creds->profile = strdup(profile_arg);
       340            LOG_INFO("Loaded AWS credentials from AWS CLI config
             (profile: %s)", profile_arg);
       341 -          return creds;
       341 +
       342 +          // Validate credentials before returning
       343 +          if (bedrock_validate_credentials(creds, profile_arg) == 1) {
       344 +              LOG_INFO("AWS credentials validated successfully");
       345 +              return creds;
       346 +          } else {
       347 +              LOG_WARN("AWS credentials from config are invalid or
           + expired");
       348 +              bedrock_creds_free(creds);
       349 +              creds = calloc(1, sizeof(AWSCredentials));
       350 +              if (!creds) {
       351 +                  LOG_ERROR("Failed to allocate AWSCredentials");
       352 +                  return NULL;
       353 +              }
       354 +              // Fall through to try SSO
       355 +          }
       356 +      } else {
       357 +          free(key_id);
       358 +          free(secret);
       359        }
       360
       361 -      free(key_id);
       362 -      free(secret);
       363 -
       361        // Try AWS SSO
       362        LOG_INFO("Attempting to load credentials via AWS SSO for
             profile: %s", profile_arg);
       363

∴ Thought for 1s (ctrl+o to show thinking)
```
- [ ] where Search tool?
- [ ] better Edit tool?
    - [ ] use bash commands to find line range
    - [ ] use bash commands to insert text at lines
- [ ] add Sleep tool. agent can wait for certain thing to happen before continuing with work


- [ ] ~/fgit/claude-c/msg-q (msg-q ✓)
$ sqlite3 .claude-c/api_calls.db 'select api_base_url, http_status, response_json from api_calls order by timestamp desc limit 1;'
https://api.openai.com|400|{
  "error": {
    "message": "An assistant message with 'tool_calls' must be followed by tool messages responding to each 'tool_call_id'. The following tool_call_ids did not have response messages: call_VJ3BU6CmJMXEQvBs5yLFGiFi",
    "type": "invalid_request_error",
    "param": "messages.[3].role",
    "code": null
  }
}
- [x] remove paste confirmation
