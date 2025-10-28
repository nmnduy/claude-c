- [ ] rewrite release.yaml
- [x] why so many linse between [Assistant] and everything else?

- [x] ctrl + l to clear input box
- [x] env var for custom log dir and api call db file
- [ ] rotation for api call db files
- [ ] pasting still fucks up the terminal
- [ ] add retries to falied API calls
- [ ] implement instruction queue. so user can keep typing

- [ ]
```sh
[2025-10-28 02:18:55] [sess_1761635777_511588dc] WARN  [indicators.h:53] get_spinner_color_tool: Using fallback color for spinner (tool)
```

```sh
[Tool: Grep] "User.*printf" in /Users/dunguyen/fgit/claude-c/src
[Assistant]
Let me search for how user input is displayed in non-TUI mode:


[Tool: Grep] "\[User\]" in /Users/dunguyen/fgit/claude-c/src
[Assistant]
Let me check the main non-TUI conversation flow:


[Tool: Read] claude.c:2900-2950
[Assistant]
Let me continue reading the main function to find the non-TUI flow:


[Tool: Read] claude.c:2950-3000

[Tool: Read] claude.c:3000-3050
[Assistant]
Now let me find the `interactive_mode` function:


[Tool: Grep] "interactive_mode" in /Users/dunguyen/fgit/claude-c/src

[Tool: Read] claude.c:2735-2800
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

- [x] large paste fails. how does node js claude prevents that and show X lines pasted.
- [x] pressing Esc will wait for tool to finish but there is no indicator
    - [ ] 'esc' doesn't show interrupted right away.
- [ ] logs are not flushed often?
- [ ] are we logging enough? are all log records go to the same log file?
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
