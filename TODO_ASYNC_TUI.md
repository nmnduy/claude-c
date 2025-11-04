# TODO: Async TUI Refactoring

**Goal**: Make the TUI always responsive by moving API calls and tool execution to background threads, allowing the input box to accept commands while AI processes requests.

**Status**: Phase 4 Complete ✅ - Ready for Phase 5
**Priority**: High
**Estimated Effort**: 3-5 days (Phase 1: ~1 day completed)

---

## Progress Summary

| Phase | Status | Description |
|-------|--------|-------------|
| Phase 1 | ✅ Complete | Message queue infrastructure (TUI + AI queues) |
| Phase 2 | ✅ Complete | Non-blocking input and TUI event loop |
| Phase 3 | ✅ Complete | Worker thread for API calls |
| Phase 4 | ✅ Complete | Async tool execution |
| Phase 5 | ⏳ Pending | Full integration |
| Phase 6 | ⏳ Pending | Testing and polish |

**Key Achievements**:
- ✅ Thread-safe circular buffer queues implemented
- ✅ 14 unit tests passing (concurrent, overflow, shutdown)
- ✅ ConversationState now protected by mutex for multi-thread access
- ✅ Non-blocking TUI event loop with persistent input buffer
- ✅ Background worker thread handles API calls + TUI message dispatch

**Next Steps**:
- Kick off Phase 5: Main-thread message dispatch integration
- Audit TUI updates to ensure all ncurses calls stay on UI thread
- Verify message queue batching and backpressure handling

---

## Current Architecture Problems

### Blocking Points
1. **Tool Execution**: `process_response()` still blocks the worker while tools run and poll via monitor thread
2. **Recursive Processing**: Tool results trigger additional API calls serially on the same worker, delaying earlier updates
3. **TUI Queue Backpressure**: Status spam during long tool runs may overflow FIFO queue without smarter batching

### User Experience Issues
- Tool progress still surfaces as coarse status changes (no per-tool streaming updates)
- Cancellation requires ESC polling inside worker; needs condition-based interrupt
- Multiple queued instructions execute strictly serially; no visibility into backlog depth
- Resize handling improved, but long tool phases still reuse last status message until completion

---

## Target Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        Main Thread                          │
│  ┌───────────────────────────────────────────────────────┐  │
│  │              TUI Event Loop (60 FPS)                  │  │
│  │  • Handle keyboard input (non-blocking)               │  │
│  │  • Process TUI message queue                          │  │
│  │  • Render conversation + input box                    │  │
│  │  • Handle resize events                               │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                              │
                              │ Message Passing
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      Worker Thread                          │
│  ┌───────────────────────────────────────────────────────┐  │
│  │           AI Processing Loop                          │  │
│  │  • Wait for instruction from queue                    │  │
│  │  • Call API (blocking OK - not main thread)           │  │
│  │  • Execute tools in parallel                          │  │
│  │  • Post updates to TUI queue                          │  │
│  │  • Handle recursive tool → API cycles                 │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

---

## Implementation Phases

### Phase 1: Message Queue Infrastructure ✅ COMPLETED

**Goal**: Create thread-safe communication channels

#### Tasks
- [x] Create `src/message_queue.h` and `src/message_queue.c`
- [x] Implement TUI message queue
  - [x] Define `TUIMessage` enum: `MSG_ADD_LINE`, `MSG_STATUS`, `MSG_CLEAR`, `MSG_ERROR`, `MSG_TODO_UPDATE`
  - [x] Implement circular buffer with mutex + condition variable
  - [x] Functions: `tui_msg_queue_init()`, `post_tui_message()`, `poll_tui_message()`, `wait_tui_message()`
  - [x] Handle overflow: drop oldest messages (FIFO eviction)
- [x] Implement AI instruction queue
  - [x] Define `AIInstruction` struct: `{char *text, ConversationState *state, priority}`
  - [x] Implement queue with pthread synchronization
  - [x] Functions: `ai_queue_init()`, `enqueue_instruction()`, `dequeue_instruction()`, `ai_queue_depth()`
  - [x] Blocking behavior when full (uses `not_full` condition variable)
- [x] Add unit tests for queues
  - [x] Test concurrent access (multiple producers/consumers) - 3x3 stress test
  - [x] Test overflow behavior - verified FIFO eviction
  - [x] Test thread safety with sanitizers - all tests pass
  - [x] Test shutdown behavior - graceful cleanup

**Files created**:
- `src/message_queue.h` (197 lines)
- `src/message_queue.c` (339 lines)
- `tests/test_message_queue.c` (485 lines)
- Updated `Makefile` to build and test

**Success Criteria**: ✅ ALL MET
- ✅ Queues pass all unit tests (14/14 passed)
- ✅ No deadlocks under stress testing (3 producers × 3 consumers × 100 messages)
- ✅ Thread-safe design with proper mutex locking
- ✅ Memory ownership documented in comments
- ✅ Graceful shutdown with condition variable signaling

**Implementation Notes**:
- TUI queue uses **FIFO eviction** on overflow (non-blocking posts)
- AI queue **blocks on full** (backpressure to prevent overwhelming worker)
- Both queues support `shutdown` flag for graceful thread termination
- Condition variables used for efficient blocking (`not_empty`, `not_full`)
- Memory ownership transfer: Queue allocates on post, caller frees on poll/dequeue
- Compatible with existing codebase: `void *conversation_state` for loose coupling

---

### Phase 2: Non-Blocking Input ✅ COMPLETED

**Goal**: Make TUI event loop non-blocking

#### Tasks
- [x] Replace blocking `tui_read_input()` with non-blocking input polling
  - [x] Use `nodelay(win, TRUE)` to make `wgetch()` non-blocking
  - [x] Return `ERR` when no input available instead of blocking
  - [x] Preserve readline-like keybindings
- [x] Create `tui_event_loop()` function
  - [x] Main loop runs at ~60 FPS with `usleep(16667)`
  - [x] Poll for input character
  - [x] Handle special keys (Ctrl+C, Ctrl+D, etc.)
  - [x] Check resize flag
  - [x] Process TUI message queue
- [x] Refactor input state management
  - [x] Move `g_input_state` from static to `TUIState->input_buffer`
  - [x] Keep input buffer persistent across frames
  - [x] Clear buffer only on submit or explicit command
- [x] Implement `submit_input()` callback
  - [x] Triggered on Enter key
  - [x] Posts user message to TUI immediately
  - [x] Provides hook for worker enqueue in Phase 3
  - [x] Clears input buffer

**Files to modify**:
- `src/tui.c` - Refactor input loop
- `src/tui.h` - Add `InputBuffer` to `TUIState`
- `src/claude.c` - Update `interactive_mode()` to use event loop

**Success Criteria**:
- ✅ TUI remains responsive while idle or processing queue messages
- ⚠️ Still blocks during API calls until worker thread (Phase 3)
- ✅ Input buffer persists across frames and redraws cleanly
- ✅ Resize handled smoothly without blocking

---

### Phase 3: Worker Thread for API Calls ✅ COMPLETED

**Goal**: Move API requests off main thread

#### Tasks
- [x] Create `src/ai_worker.h` and `src/ai_worker.c`
- [x] Define `AIWorkerContext` struct
  - [x] Thread handle
  - [x] Instruction queue
  - [x] TUI message queue (for posting updates)
  - [x] Running flag
  - [x] ConversationState pointer (shared, needs mutex)
- [x] Implement worker thread function
  - [x] Main loop: `dequeue_instruction()` → process → repeat
  - [x] Post status updates: "Waiting for API...", "Processing tools...", etc.
  - [x] Call `call_api()` (still blocking, but OK in worker)
  - [x] Post assistant response to TUI
  - [x] Handle API errors gracefully
- [x] Add synchronization to `ConversationState`
  - [x] Add `pthread_mutex_t conv_mutex` to struct
  - [x] Lock when adding messages
  - [x] Lock when building API requests
  - [x] Document lock ordering to prevent deadlocks
- [x] Implement worker lifecycle
  - [x] `ai_worker_start()` - Create thread
  - [x] `ai_worker_stop()` - Signal shutdown + join
  - [x] `ai_worker_submit()` - Enqueue instruction
- [x] Update `interactive_mode()`
  - [x] Start worker thread at initialization
  - [x] On Enter key: call `ai_worker_submit()` instead of `call_api()`
  - [x] Stop worker thread at cleanup

**Files created**:
- `src/ai_worker.h`
- `src/ai_worker.c`

**Files modified**:
- `src/claude_internal.h` - Added mutex to `ConversationState`
- `src/claude.c` - Worker integration, UI helpers, mutex usage
- `src/openai_messages.c` - Request builder uses conversation lock
- Tests updated to init/destroy conversation mutex

**Success Criteria**:
- ✅ API calls do not block TUI input/render loop
- ✅ User can type next instruction while API processes
- ✅ Conversation history updates correctly via worker
- ⚠️ Need dedicated TSan run to confirm no data races
- ✅ Worker thread cleans up properly on exit

---

### Phase 4: Async Tool Execution ✅

**Goal**: Remove blocking wait for tool threads

#### Tasks
- [x] Modify `process_response()` to be async-friendly
  - [x] Remove `while (!all_tools_done)` busy loop
  - [x] Use condition variable to signal completion
  - [x] Worker waits on condition instead of polling
- [x] Create tool completion callback system
  - [x] Define `ToolCompletion` struct: `{tool_name, result, is_error}`
  - [x] Each tool thread signals completion via callback
  - [x] Callback posts status to TUI queue
- [x] Refactor tool monitoring
  - [x] Replace monitor thread with condition variable
  - [x] Worker thread waits: `pthread_cond_wait(&tools_done_cond, &lock)`
  - [x] Last tool to complete signals: `pthread_cond_broadcast(&tools_done_cond)`
- [x] Handle ESC interrupt gracefully
  - [x] Check interrupt flag in worker thread
  - [x] Cancel tool threads if needed
  - [x] Post cancellation message to TUI

**Files to modify**:
- `src/claude.c` - Refactor `process_response()` and tool execution
- `src/ai_worker.c` - Add tool completion handling

**Success Criteria**:
- Tool execution does not block TUI
- Status updates appear in real-time during tool runs
- ESC cancellation works without busy loop
- No deadlocks in tool synchronization

---

### Phase 5: Full Integration ⏳

**Goal**: Wire everything together and test end-to-end

#### Tasks
- [ ] Update all TUI update calls
  - [ ] Replace direct `tui_add_conversation_line()` with `post_tui_message()`
  - [ ] Replace `tui_update_status()` with queue posts
  - [ ] Ensure only main thread calls ncurses functions
- [ ] Implement TUI message processing
  - [ ] `process_tui_message_queue()` in event loop
  - [ ] Dispatch messages: add line, update status, clear, error
  - [ ] Batch process multiple messages per frame (rate limiting)
- [ ] Handle edge cases
  - [ ] Worker queue full - block or notify user?
  - [ ] TUI queue full - drop oldest messages
  - [ ] API error during processing - show in TUI
  - [ ] Multiple resize events - coalesce
- [ ] Add graceful shutdown
  - [ ] Signal worker to stop
  - [ ] Wait for current instruction to complete
  - [ ] Drain message queues
  - [ ] Clean up all resources
- [ ] Memory ownership rules
  - [ ] Document who owns what in ASYNC_DESIGN.md
  - [ ] Worker owns instruction after dequeue
  - [ ] TUI owns messages after poll
  - [ ] ConversationState shared with mutex

**Files to modify**:
- `src/tui.c` - Message queue processing
- `src/claude.c` - Replace blocking calls
- `src/ai_worker.c` - Complete integration

**New documentation**:
- `docs/ASYNC_DESIGN.md` - Architecture and ownership rules

**Success Criteria**:
- Full user workflow works: type → queue → process → respond → repeat
- Can queue multiple instructions
- TUI remains responsive throughout
- No memory leaks (Valgrind clean)
- No data races (TSan clean)
- ESC cancellation works at any point
- Graceful shutdown without hangs

---

### Phase 6: Testing and Polish ⏳

**Goal**: Ensure production quality

#### Tasks
- [ ] Stress testing
  - [ ] Submit 10 rapid instructions
  - [ ] Long-running tool execution (sleep tool)
  - [ ] Resize during processing
  - [ ] Rapid resize events
- [ ] Error handling
  - [ ] Network timeout during API call
  - [ ] Out of memory
  - [ ] Queue overflow
  - [ ] Tool crash/timeout
- [ ] User experience improvements
  - [ ] Show queue depth in status line
  - [ ] Visual indicator when AI is processing
  - [ ] Ability to cancel queued (not yet started) instructions
  - [ ] Clear error messages
- [ ] Performance tuning
  - [ ] Measure frame time - should be <16ms
  - [ ] Profile lock contention
  - [ ] Optimize message queue access
- [ ] Documentation
  - [ ] Update README with new architecture
  - [ ] Add ASYNC_DESIGN.md with diagrams
  - [ ] Document threading model
  - [ ] Add troubleshooting section

**Testing checklist**:
- [ ] Run with `-fsanitize=address,undefined,thread`
- [ ] Valgrind memcheck - zero leaks
- [ ] Helgrind - no data races
- [ ] Test on Linux and macOS
- [ ] Test with small terminal (80x24)
- [ ] Test with large terminal (200x60)

**Success Criteria**:
- All tests pass
- Zero crashes in 1-hour stress test
- User feedback positive
- Code review complete
- Documentation reviewed

---

## Non-Functional Requirements

### Performance
- TUI frame time < 16ms (60 FPS)
- Input latency < 50ms
- No noticeable lag during rendering

### Reliability
- Zero deadlocks
- Zero data races
- Graceful error handling
- Clean shutdown under all conditions

### Maintainability
- Clear separation of concerns (UI vs. business logic)
- Well-documented threading model
- Unit tests for all queues and workers
- Integration tests for full workflow

---

## Future Enhancements (Out of Scope)

These are nice-to-haves but not required for initial release:

- [ ] Multiple worker threads (parallel AI processing)
- [ ] Priority queue (interrupt current task with new one)
- [ ] History browsing (view past conversations)
- [ ] Save/load conversation state
- [ ] Streaming API responses (partial text as it arrives)
- [ ] Progress bars for tool execution
- [ ] Split view (multiple conversations)
- [ ] Vim keybindings mode
- [ ] Mouse support for scrolling

---

## Risk Assessment

### High Risk
- **Deadlocks**: Multiple mutexes (conversation state, queues) - need careful lock ordering
- **Race conditions**: Shared state between threads - requires thorough testing with TSan
- **Memory leaks**: Complex ownership rules - needs Valgrind verification

### Medium Risk
- **Performance**: Mutex contention could cause frame drops - may need lockless queue
- **Complexity**: Async code harder to debug - need good logging
- **Testing**: Hard to reproduce timing-dependent bugs

### Mitigation Strategies
- Use `-fsanitize=thread` from day 1
- Add extensive logging with thread IDs
- Use condition variables instead of busy loops
- Document lock ordering explicitly
- Write unit tests for all threading primitives
- Use RAII-like patterns (`pthread_cleanup_push`)

---

## Definition of Done

### Phase 1-2 (Foundation)
- [ ] Message queues implemented and tested
- [ ] TUI event loop non-blocking
- [ ] Input works without blocking

### Phase 3-4 (Worker Thread)
- [ ] API calls moved to worker thread
- [ ] Tool execution non-blocking
- [ ] TUI updates from worker via queue

### Phase 5-6 (Polish)
- [ ] Full integration working
- [ ] All tests passing
- [ ] Zero sanitizer warnings
- [ ] Documentation complete

### Final Acceptance
- [ ] Can queue 5 instructions while first is processing
- [ ] Input box always responsive
- [ ] Resize works during processing
- [ ] ESC cancellation works
- [ ] No crashes in 1-hour stress test
- [ ] Code review approved
- [ ] User acceptance testing passed

---

## Dependencies

### External
- pthread (already used)
- ncurses (already used)
- Existing queue implementations (if any)

### Internal
- TUI module must be stable
- ConversationState structure may need changes
- API call code must remain thread-safe

---

## Rollback Plan

If async refactoring causes too many issues:

1. **Keep changes in feature branch** - don't merge to main
2. **Alternative**: Simpler non-blocking input with spinner
   - Make `wgetch()` non-blocking with timeout
   - Check for input during API wait (already partially done)
   - Less ambitious but lower risk
3. **Revert**: Git rollback to pre-refactoring state if needed

---

## Resources

### Documentation to Write
- `docs/ASYNC_DESIGN.md` - Architecture overview
- `docs/THREADING_MODEL.md` - Lock ordering, ownership rules
- `docs/MESSAGE_QUEUE_API.md` - Queue usage guide

### Similar Projects (for reference)
- tmux source code - event loop + worker threads
- htop - non-blocking TUI with updates
- vim - async job control

### Testing Tools
- ThreadSanitizer (`-fsanitize=thread`)
- Valgrind Helgrind (race detection)
- stress-ng (stress testing)

---

**Implementation Owner**: TBD (assign to developer)
**Created**: 2025-11-03
**Last Updated**: 2025-11-03
**Implementation Status**: Phase 1 complete, Phase 2 ready to start
