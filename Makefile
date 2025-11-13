# Makefile for Claude Code - Pure C Edition

CC ?= gcc
CLANG = clang
CFLAGS = -Werror -Wall -Wextra -Wpedantic -Wformat=2 -Wconversion -Wshadow -Wcast-qual -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wuninitialized -Warray-bounds -Wvla -Wwrite-strings -Wnull-dereference -Wimplicit-fallthrough -Wsign-conversion -Wsign-compare -Wfloat-equal -Wpointer-arith -Wbad-function-cast -Wstrict-overflow -Waggregate-return -Wredundant-decls -Wnested-externs -Winline -Wswitch-enum -Wswitch-default -Wenum-conversion -Wdisabled-optimization -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE=1 -Wno-aggregate-return $(SANITIZERS)
DEBUG_CFLAGS = -Werror -Wall -Wextra -Wpedantic -Wformat=2 -Wconversion -Wshadow -Wcast-qual -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wuninitialized -Warray-bounds -Wvla -Wwrite-strings -Wnull-dereference -Wimplicit-fallthrough -Wsign-conversion -Wsign-compare -Wfloat-equal -Wpointer-arith -Wbad-function-cast -Wstrict-overflow -Waggregate-return -Wredundant-decls -Wnested-externs -Winline -Wswitch-enum -Wswitch-default -Wenum-conversion -Wdisabled-optimization -g -O0 -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE=1 -fsanitize=address -fno-omit-frame-pointer
# Detect OS for ncurses library linking
UNAME_S := $(shell uname -s)

# Default ncurses library (Linux)
NCURSES_LIB = -lncursesw

ifeq ($(UNAME_S),Darwin)
    # macOS typically uses just ncurses (not ncursesw)
    NCURSES_LIB = -lncurses
endif

LDFLAGS = -lcurl -lpthread -lsqlite3 -lssl -lcrypto $(NCURSES_LIB) $(SANITIZERS)
DEBUG_LDFLAGS = -lcurl -lpthread -lsqlite3 -lssl -lcrypto $(NCURSES_LIB) -fsanitize=address

# Installation prefix (can be overridden via command line)
INSTALL_PREFIX ?= $(HOME)/.local

# Version management
VERSION_FILE := VERSION
VERSION := $(shell cat $(VERSION_FILE) 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date +%Y-%m-%d)
VERSION_H := src/version.h

# Detect OS for library linking
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    # macOS - check for Homebrew installation
    HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null)
    ifeq ($(HOMEBREW_PREFIX),)
        # Fallback to common Homebrew paths if brew command not found
        ifeq ($(shell uname -m),arm64)
            # Apple Silicon (M1/M2/M3)
            HOMEBREW_PREFIX := /opt/homebrew
        else
            # Intel Mac
            HOMEBREW_PREFIX := /usr/local
        endif
    endif
    CFLAGS += -I$(HOMEBREW_PREFIX)/include
    DEBUG_CFLAGS += -I$(HOMEBREW_PREFIX)/include
    LDFLAGS += -L$(HOMEBREW_PREFIX)/lib -lcjson
    DEBUG_LDFLAGS += -L$(HOMEBREW_PREFIX)/lib -lcjson
else ifeq ($(UNAME_S),Linux)
    # Linux
    LDFLAGS += -lcjson
    DEBUG_LDFLAGS += -lcjson
endif

# Optional voice input (PortAudio). VOICE=auto|1|0
VOICE ?= auto
HAVE_PKGCONFIG := $(shell command -v pkg-config >/dev/null 2>&1 && echo yes || echo no)
ifeq ($(VOICE),1)
    CFLAGS += -DHAVE_PORTAUDIO=1
    LDFLAGS += -lportaudio
    DEBUG_LDFLAGS += -lportaudio
else ifeq ($(VOICE),0)
    CFLAGS += -DDISABLE_VOICE=1
else
    # auto-detect via pkg-config if available
    ifeq ($(HAVE_PKGCONFIG),yes)
        ifeq ($(shell pkg-config --exists portaudio-2.0 && echo yes || echo no),yes)
            CFLAGS += -DHAVE_PORTAUDIO=1 $(shell pkg-config --cflags portaudio-2.0)
            LDFLAGS += $(shell pkg-config --libs portaudio-2.0)
            DEBUG_LDFLAGS += $(shell pkg-config --libs portaudio-2.0)
        else
            CFLAGS += -DDISABLE_VOICE=1
        endif
    else
        CFLAGS += -DDISABLE_VOICE=1
    endif
endif

BUILD_DIR = build
TARGET = $(BUILD_DIR)/claude-c
TEST_EDIT_TARGET = $(BUILD_DIR)/test_edit
TEST_READ_TARGET = $(BUILD_DIR)/test_read
TEST_TODO_TARGET = $(BUILD_DIR)/test_todo
TEST_TODO_WRITE_TARGET = $(BUILD_DIR)/test_todo_write
TEST_TIMING_TARGET = $(BUILD_DIR)/test_tool_timing
TEST_PASTE_TARGET = $(BUILD_DIR)/test_paste
TEST_RETRY_JITTER_TARGET = $(BUILD_DIR)/test_retry_jitter
TEST_OPENAI_FORMAT_TARGET = $(BUILD_DIR)/test_openai_format
TEST_WRITE_DIFF_INTEGRATION_TARGET = $(BUILD_DIR)/test_write_diff_integration
TEST_ROTATION_TARGET = $(BUILD_DIR)/test_rotation
TEST_PATCH_PARSER_TARGET = $(BUILD_DIR)/test_patch_parser
TEST_THREAD_CANCEL_TARGET = $(BUILD_DIR)/test_thread_cancel
TEST_AWS_CRED_ROTATION_TARGET = $(BUILD_DIR)/test_aws_credential_rotation
TEST_MESSAGE_QUEUE_TARGET = $(BUILD_DIR)/test_message_queue
TEST_EVENT_LOOP_TARGET = $(BUILD_DIR)/test_event_loop
TEST_TEXT_WRAP_TARGET = $(BUILD_DIR)/test_text_wrap
TEST_MCP_TARGET = $(BUILD_DIR)/test_mcp
TEST_WM_TARGET = $(BUILD_DIR)/test_window_manager
QUERY_TOOL = $(BUILD_DIR)/query_logs
SRC = src/claude.c
LOGGER_SRC = src/logger.c
LOGGER_OBJ = $(BUILD_DIR)/logger.o
PERSISTENCE_SRC = src/persistence.c
PERSISTENCE_OBJ = $(BUILD_DIR)/persistence.o
MIGRATIONS_SRC = src/migrations.c
MIGRATIONS_OBJ = $(BUILD_DIR)/migrations.o
COMMANDS_SRC = src/commands.c
COMMANDS_OBJ = $(BUILD_DIR)/commands.o
COMPLETION_SRC = src/completion.c
COMPLETION_OBJ = $(BUILD_DIR)/completion.o
TUI_SRC = src/tui.c
TUI_OBJ = $(BUILD_DIR)/tui.o
HISTORY_FILE_SRC = src/history_file.c
HISTORY_FILE_OBJ = $(BUILD_DIR)/history_file.o
TODO_SRC = src/todo.c
TODO_OBJ = $(BUILD_DIR)/todo.o
AWS_BEDROCK_SRC = src/aws_bedrock.c
AWS_BEDROCK_OBJ = $(BUILD_DIR)/aws_bedrock.o
PROVIDER_SRC = src/provider.c
PROVIDER_OBJ = $(BUILD_DIR)/provider.o
OPENAI_PROVIDER_SRC = src/openai_provider.c
OPENAI_PROVIDER_OBJ = $(BUILD_DIR)/openai_provider.o
OPENAI_MESSAGES_SRC = src/openai_messages.c
OPENAI_MESSAGES_OBJ = $(BUILD_DIR)/openai_messages.o
BEDROCK_PROVIDER_SRC = src/bedrock_provider.c
BEDROCK_PROVIDER_OBJ = $(BUILD_DIR)/bedrock_provider.o
BUILTIN_THEMES_SRC = src/builtin_themes.c
BUILTIN_THEMES_OBJ = $(BUILD_DIR)/builtin_themes.o
PATCH_PARSER_SRC = src/patch_parser.c
PATCH_PARSER_OBJ = $(BUILD_DIR)/patch_parser.o
MESSAGE_QUEUE_SRC = src/message_queue.c
MESSAGE_QUEUE_OBJ = $(BUILD_DIR)/message_queue.o
AI_WORKER_SRC = src/ai_worker.c
AI_WORKER_OBJ = $(BUILD_DIR)/ai_worker.o
VOICE_INPUT_SRC = src/voice_input.c
VOICE_INPUT_OBJ = $(BUILD_DIR)/voice_input.o
MCP_SRC = src/mcp.c
MCP_OBJ = $(BUILD_DIR)/mcp.o
WINDOW_MANAGER_SRC = src/window_manager.c
WINDOW_MANAGER_OBJ = $(BUILD_DIR)/window_manager.o
TOOL_UTILS_SRC = src/tool_utils.c
TOOL_UTILS_OBJ = $(BUILD_DIR)/tool_utils.o
TEST_EDIT_SRC = tests/test_edit.c
TEST_READ_SRC = tests/test_read.c
TEST_TODO_SRC = tests/test_todo.c
TEST_TODO_WRITE_SRC = tests/test_todo_write.c
TEST_PASTE_SRC = tests/test_paste.c
TEST_RETRY_JITTER_SRC = tests/test_retry_jitter.c
TEST_OPENAI_FORMAT_SRC = tests/test_openai_format.c
TEST_WRITE_DIFF_INTEGRATION_SRC = tests/test_write_diff_integration.c
TEST_ROTATION_SRC = tests/test_rotation.c
TEST_PATCH_PARSER_SRC = tests/test_patch_parser.c
TEST_THREAD_CANCEL_SRC = tests/test_thread_cancel.c
TEST_AWS_CRED_ROTATION_SRC = tests/test_aws_credential_rotation.c
TEST_MESSAGE_QUEUE_SRC = tests/test_message_queue.c
TEST_EVENT_LOOP_SRC = tests/test_event_loop.c
TEST_STUBS_SRC = tests/test_stubs.c
TEST_MCP_SRC = tests/test_mcp.c
TEST_WM_SRC = tests/test_window_manager.c
TEST_CANCEL_FLOW_TARGET = $(BUILD_DIR)/test_cancel_flow
TEST_BASH_SUMMARY_TARGET = $(BUILD_DIR)/test_bash_summary
TEST_BASH_SUMMARY_SRC = tests/test_bash_summary.c

.PHONY: all clean check-deps install test test-edit test-read test-todo test-todo-write test-paste test-retry-jitter test-openai-format test-write-diff-integration test-rotation test-patch-parser test-thread-cancel test-aws-cred-rotation test-message-queue test-event-loop test-wrap test-mcp test-bash-summary query-tool debug analyze sanitize-ub sanitize-all sanitize-leak valgrind memscan version show-version update-version bump-version bump-patch build clang ci-test ci-gcc ci-clang ci-gcc-sanitize ci-clang-sanitize ci-all fmt-whitespace

all: check-deps $(TARGET)

build: check-deps $(TARGET)

clang: check-deps $(BUILD_DIR)/claude-c-clang

debug: check-deps $(BUILD_DIR)/claude-c-debug

query-tool: check-deps $(QUERY_TOOL)

test: test-edit test-read test-todo test-paste test-timing test-openai-format test-write-diff-integration test-rotation test-patch-parser test-thread-cancel test-aws-cred-rotation test-message-queue test-wrap test-mcp test-wm test-bash-summary test-cancel-flow

test-edit: check-deps $(TEST_EDIT_TARGET)
	@echo ""
	@echo "Running Edit tool tests..."
	@echo ""
	@./$(TEST_EDIT_TARGET)

test-read: check-deps $(TEST_READ_TARGET)
	@echo ""
	@echo "Running Read tool tests..."
	@echo ""
	@./$(TEST_READ_TARGET)

test-todo: check-deps $(TEST_TODO_TARGET)
	@echo ""
	@echo "Running TODO list tests..."
	@echo ""
	@./$(TEST_TODO_TARGET)

test-todo-write: check-deps $(TEST_TODO_WRITE_TARGET)
	@echo ""
	@echo "Running TodoWrite tool tests..."
	@echo ""
	@./$(TEST_TODO_WRITE_TARGET)

test-paste: check-deps $(TEST_PASTE_TARGET)
	@echo ""
	@echo "Running Paste Handler tests..."
	@echo ""
	@./$(TEST_PASTE_TARGET)

test-retry-jitter: check-deps $(TEST_RETRY_JITTER_TARGET)
	@echo ""
	@echo "Running Retry Jitter tests..."
	@echo ""
	@./$(TEST_RETRY_JITTER_TARGET)

test-timing: check-deps $(TEST_TIMING_TARGET)
	@echo ""
	@echo "Running tool timing tests..."
	@echo ""
	@./$(TEST_TIMING_TARGET)

test-openai-format: check-deps $(TEST_OPENAI_FORMAT_TARGET)
	@echo ""
	@echo "Running OpenAI message format validation tests..."
	@echo ""
	@./$(TEST_OPENAI_FORMAT_TARGET)

test-write-diff-integration: check-deps $(TEST_WRITE_DIFF_INTEGRATION_TARGET)
	@echo ""
	@echo "Running Write tool diff integration tests..."
	@echo ""
	@./$(TEST_WRITE_DIFF_INTEGRATION_TARGET)

test-rotation: check-deps $(TEST_ROTATION_TARGET)
	@echo ""
	@echo "Running database rotation tests..."
	@echo ""
	@./$(TEST_ROTATION_TARGET)

test-patch-parser: check-deps $(TEST_PATCH_PARSER_TARGET)
	@echo ""
	@echo "Running Patch Parser tests..."
	@echo ""
	@./$(TEST_PATCH_PARSER_TARGET)

test-thread-cancel: check-deps $(TEST_THREAD_CANCEL_TARGET)
	@echo ""
	@echo "Running Thread Cancellation tests..."
	@echo ""
	@./$(TEST_THREAD_CANCEL_TARGET)

test-aws-cred-rotation: check-deps $(TEST_AWS_CRED_ROTATION_TARGET)
	@echo ""
	@echo "Running AWS Credential Rotation tests..."
	@echo ""
	@./$(TEST_AWS_CRED_ROTATION_TARGET)

test-message-queue: check-deps $(TEST_MESSAGE_QUEUE_TARGET)
	@echo ""
	@echo "Running Message Queue tests..."
	@echo ""
	@./$(TEST_MESSAGE_QUEUE_TARGET)

test-event-loop: check-deps $(TEST_EVENT_LOOP_TARGET)
	@echo ""
	@echo "Running Event Loop test (interactive)..."
	@echo "Type some text and press Enter. Type 'quit' to exit."
	@echo ""
	@./$(TEST_EVENT_LOOP_TARGET)

test-wrap: check-deps $(TEST_TEXT_WRAP_TARGET)
	@echo ""
	@echo "Running Text Wrapping tests..."
	@echo ""
	@./$(TEST_TEXT_WRAP_TARGET)

test-mcp: check-deps $(TEST_MCP_TARGET)
	@echo ""
	@echo "Running MCP integration tests..."
	@echo ""
	@./$(TEST_MCP_TARGET)

test-wm: check-deps $(TEST_WM_TARGET)
	@echo ""
	@echo "Running Window Manager tests..."
	@echo ""
	@./$(TEST_WM_TARGET)

$(TARGET): $(SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(COMMANDS_OBJ) $(COMPLETION_OBJ) $(TUI_OBJ) $(WINDOW_MANAGER_OBJ) $(TODO_OBJ) $(AWS_BEDROCK_OBJ) $(PROVIDER_OBJ) $(OPENAI_PROVIDER_OBJ) $(OPENAI_MESSAGES_OBJ) $(BEDROCK_PROVIDER_OBJ) $(BUILTIN_THEMES_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ) $(AI_WORKER_OBJ) $(VOICE_INPUT_OBJ) $(MCP_OBJ) $(TOOL_UTILS_OBJ) $(HISTORY_FILE_OBJ) $(VERSION_H)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(COMMANDS_OBJ) $(COMPLETION_OBJ) $(TUI_OBJ) $(WINDOW_MANAGER_OBJ) $(TODO_OBJ) $(AWS_BEDROCK_OBJ) $(PROVIDER_OBJ) $(OPENAI_PROVIDER_OBJ) $(OPENAI_MESSAGES_OBJ) $(BEDROCK_PROVIDER_OBJ) $(BUILTIN_THEMES_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ) $(AI_WORKER_OBJ) $(VOICE_INPUT_OBJ) $(MCP_OBJ) $(TOOL_UTILS_OBJ) $(HISTORY_FILE_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Build successful!"
	@echo "Version: $(VERSION)"
	@echo "Run: ./$(TARGET) \"your prompt here\""
	@echo ""

# Generate version.h from VERSION file
$(VERSION_H): $(VERSION_FILE)
	@echo "Generating version.h..."
	@mkdir -p src
	@# Parse version string (e.g., "1.2.3-beta.1" -> 1, 2, 3)
	@VERSION_MAJOR=$$(echo "$(VERSION)" | sed 's/^\([0-9]*\)\..*/\1/' | head -1); \
	VERSION_MINOR=$$(echo "$(VERSION)" | sed 's/^[0-9]*\.\([0-9]*\)\..*/\1/' | head -1); \
	VERSION_PATCH=$$(echo "$(VERSION)" | sed 's/^[0-9]*\.[0-9]*\.\([0-9]*\).*/\1/' | head -1); \
	VERSION_NUMBER=$$(printf "%d" $$(($$VERSION_MAJOR * 65536 + $$VERSION_MINOR * 256 + $$VERSION_PATCH))); \
	printf "0x%06x" $$VERSION_NUMBER > /tmp/version_num.tmp; \
	VERSION_HEX=$$(cat /tmp/version_num.tmp); \
	rm -f /tmp/version_num.tmp; \
	{ \
	echo "/*"; \
	echo " * version.h - Central version management for Claude C"; \
	echo " *"; \
	echo " * This file provides a single source of truth for version information."; \
	echo " * It's automatically generated from the VERSION file during build."; \
	echo " */"; \
	echo ""; \
	echo "#ifndef VERSION_H"; \
	echo "#define VERSION_H"; \
	echo ""; \
	echo "// Version string (e.g., \"0.0.2\", \"1.0.0\", \"1.2.3-beta.1\")"; \
	echo "#define CLAUDE_C_VERSION \"$(VERSION)\""; \
	echo ""; \
	echo "// Version components for programmatic use"; \
	echo "#define CLAUDE_C_VERSION_MAJOR $$VERSION_MAJOR"; \
	echo "#define CLAUDE_C_VERSION_MINOR $$VERSION_MINOR"; \
	echo "#define CLAUDE_C_VERSION_PATCH $$VERSION_PATCH"; \
	echo ""; \
	echo "// Version as numeric value for comparisons (e.g., 0x000002)"; \
	echo "#define CLAUDE_C_VERSION_NUMBER $$VERSION_HEX"; \
	echo ""; \
	echo "// Build timestamp (automatically generated)"; \
	echo "#define CLAUDE_C_BUILD_TIMESTAMP \"$(BUILD_DATE)\""; \
	echo ""; \
	echo "// Full version string with build info"; \
	echo "#define CLAUDE_C_VERSION_FULL \"$(VERSION) (built $(BUILD_DATE))\""; \
	echo ""; \
	echo "#endif // VERSION_H"; \
	} > $(VERSION_H)
	@echo "✓ Version: $(VERSION)"

# Debug build with AddressSanitizer for finding memory bugs
$(BUILD_DIR)/claude-c-debug: $(SRC) $(LOGGER_SRC) $(PERSISTENCE_SRC) $(MIGRATIONS_SRC) $(COMMANDS_SRC) $(COMPLETION_SRC) $(TUI_SRC) $(TODO_SRC) $(AWS_BEDROCK_SRC) $(PROVIDER_SRC) $(OPENAI_PROVIDER_SRC) $(OPENAI_MESSAGES_SRC) $(BEDROCK_PROVIDER_SRC) $(BUILTIN_THEMES_SRC) $(PATCH_PARSER_SRC) $(MESSAGE_QUEUE_SRC) $(AI_WORKER_SRC) $(VOICE_INPUT_SRC) $(MCP_SRC) $(TOOL_UTILS_SRC)
	@mkdir -p $(BUILD_DIR)
	@echo "Building with AddressSanitizer (debug mode)..."
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/logger_debug.o $(LOGGER_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/migrations_debug.o $(MIGRATIONS_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/persistence_debug.o $(PERSISTENCE_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/commands_debug.o $(COMMANDS_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/completion_debug.o $(COMPLETION_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/tui_debug.o $(TUI_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/todo_debug.o $(TODO_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/aws_bedrock_debug.o $(AWS_BEDROCK_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/provider_debug.o $(PROVIDER_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/openai_provider_debug.o $(OPENAI_PROVIDER_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/openai_messages_debug.o $(OPENAI_MESSAGES_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/bedrock_provider_debug.o $(BEDROCK_PROVIDER_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/builtin_themes_debug.o $(BUILTIN_THEMES_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/patch_parser_debug.o $(PATCH_PARSER_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/message_queue_debug.o $(MESSAGE_QUEUE_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/ai_worker_debug.o $(AI_WORKER_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/voice_input_debug.o $(VOICE_INPUT_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/mcp_debug.o $(MCP_SRC)
	$(CC) $(DEBUG_CFLAGS) -o $(BUILD_DIR)/claude-c-debug $(SRC) $(BUILD_DIR)/logger_debug.o $(BUILD_DIR)/persistence_debug.o $(BUILD_DIR)/migrations_debug.o $(BUILD_DIR)/commands_debug.o $(BUILD_DIR)/completion_debug.o $(BUILD_DIR)/tui_debug.o $(BUILD_DIR)/todo_debug.o $(BUILD_DIR)/aws_bedrock_debug.o $(BUILD_DIR)/provider_debug.o $(BUILD_DIR)/openai_provider_debug.o $(BUILD_DIR)/openai_messages_debug.o $(BUILD_DIR)/bedrock_provider_debug.o $(BUILD_DIR)/builtin_themes_debug.o $(BUILD_DIR)/patch_parser_debug.o $(BUILD_DIR)/message_queue_debug.o $(BUILD_DIR)/ai_worker_debug.o $(BUILD_DIR)/voice_input_debug.o $(BUILD_DIR)/mcp_debug.o $(TOOL_UTILS_SRC) $(DEBUG_LDFLAGS)
	@echo ""
	@echo "✓ Debug build successful with AddressSanitizer!"
	@echo "Run: ./$(BUILD_DIR)/claude-c-debug \"your prompt here\""
	@echo ""
	@echo "AddressSanitizer will detect:"
	@echo "  - Use-after-free"
	@echo "  - Double-free"
	@echo "  - Heap/stack buffer overflows"
	@echo "  - Memory leaks"
	@echo ""

# Build with clang compiler
$(BUILD_DIR)/claude-c-clang: $(SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(COMMANDS_OBJ) $(COMPLETION_OBJ) $(TUI_OBJ) $(WINDOW_MANAGER_OBJ) $(TODO_OBJ) $(AWS_BEDROCK_OBJ) $(PROVIDER_OBJ) $(OPENAI_PROVIDER_OBJ) $(OPENAI_MESSAGES_OBJ) $(BEDROCK_PROVIDER_OBJ) $(BUILTIN_THEMES_OBJ) $(PATCH_PARSER_OBJ) $(AI_WORKER_OBJ) $(MESSAGE_QUEUE_OBJ) $(VOICE_INPUT_OBJ) $(MCP_OBJ) $(TOOL_UTILS_SRC) $(VERSION_H)
	@mkdir -p $(BUILD_DIR)
	@echo "Building with clang compiler..."
	$(CLANG) $(CFLAGS) -o $(BUILD_DIR)/claude-c-clang $(SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(COMMANDS_OBJ) $(COMPLETION_OBJ) $(TUI_OBJ) $(WINDOW_MANAGER_OBJ) $(TODO_OBJ) $(AWS_BEDROCK_OBJ) $(PROVIDER_OBJ) $(OPENAI_PROVIDER_OBJ) $(OPENAI_MESSAGES_OBJ) $(BEDROCK_PROVIDER_OBJ) $(BUILTIN_THEMES_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ) $(AI_WORKER_OBJ) $(VOICE_INPUT_OBJ) $(MCP_OBJ) $(TOOL_UTILS_SRC) $(LDFLAGS)
	@echo ""
	@echo "✓ Clang build successful!"
	@echo "Version: $(VERSION)"
	@echo "Run: ./$(BUILD_DIR)/claude-c-clang \"your prompt here\""
	@echo ""

# Static analysis with compiler's built-in analyzer
analyze: check-deps
	@mkdir -p $(BUILD_DIR)
	@echo "Running static analysis..."
	@echo ""
	@if command -v clang >/dev/null 2>&1; then \
		echo "Using clang --analyze..."; \
		clang --analyze -Xanalyzer -analyzer-output=text $(CFLAGS) $(SRC) $(LOGGER_SRC) $(PERSISTENCE_SRC) $(MIGRATIONS_SRC) 2>&1 | tee $(BUILD_DIR)/analyze.log; \
	else \
		echo "Using gcc -fanalyzer..."; \
		gcc -fanalyzer $(CFLAGS) -c $(SRC) $(LOGGER_SRC) $(PERSISTENCE_SRC) $(MIGRATIONS_SRC) 2>&1 | tee $(BUILD_DIR)/analyze.log; \
	fi
	@echo ""
	@echo "✓ Static analysis complete. Results saved to $(BUILD_DIR)/analyze.log"
	@echo ""

# Build with Undefined Behavior Sanitizer
sanitize-ub: check-deps
	@mkdir -p $(BUILD_DIR)
	@echo "Building with Undefined Behavior Sanitizer..."
	$(CC) $(CFLAGS) -g -O0 -fsanitize=undefined -fno-omit-frame-pointer -c -o $(BUILD_DIR)/logger_ub.o $(LOGGER_SRC)
	$(CC) $(CFLAGS) -g -O0 -fsanitize=undefined -fno-omit-frame-pointer -c -o $(BUILD_DIR)/migrations_ub.o $(MIGRATIONS_SRC)
	$(CC) $(CFLAGS) -g -O0 -fsanitize=undefined -fno-omit-frame-pointer -c -o $(BUILD_DIR)/persistence_ub.o $(PERSISTENCE_SRC)
	$(CC) $(CFLAGS) -g -O0 -fsanitize=undefined -fno-omit-frame-pointer -o $(BUILD_DIR)/claude-c-ubsan $(SRC) $(BUILD_DIR)/logger_ub.o $(BUILD_DIR)/persistence_ub.o $(BUILD_DIR)/migrations_ub.o $(LDFLAGS) -fsanitize=undefined
	@echo ""
	@echo "✓ Build successful with UBSan!"
	@echo "Run: ./$(BUILD_DIR)/claude-c-ubsan \"your prompt here\""
	@echo ""

# Build with combined Address + Undefined Behavior Sanitizers (recommended)
sanitize-all: check-deps
	@mkdir -p $(BUILD_DIR)
	@echo "Building with Address + Undefined Behavior Sanitizers (recommended for testing)..."
	$(CC) $(CFLAGS) -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -c -o $(BUILD_DIR)/logger_all.o $(LOGGER_SRC)
	$(CC) $(CFLAGS) -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -c -o $(BUILD_DIR)/migrations_all.o $(MIGRATIONS_SRC)
	$(CC) $(CFLAGS) -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -c -o $(BUILD_DIR)/persistence_all.o $(PERSISTENCE_SRC)
	$(CC) $(CFLAGS) -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer -o $(BUILD_DIR)/claude-c-allsan $(SRC) $(BUILD_DIR)/logger_all.o $(BUILD_DIR)/persistence_all.o $(BUILD_DIR)/migrations_all.o $(LDFLAGS) -fsanitize=address,undefined
	@echo ""
	@echo "✓ Build successful with combined sanitizers!"
	@echo "Run: ./$(BUILD_DIR)/claude-c-allsan \"your prompt here\""
	@echo ""
	@echo "This build detects:"
	@echo "  - Use-after-free, double-free, buffer overflows (AddressSanitizer)"
	@echo "  - Undefined behavior, integer overflows, null dereferences (UBSan)"
	@echo ""

# Build with Leak Sanitizer only
sanitize-leak: check-deps
	@mkdir -p $(BUILD_DIR)
	@echo "Building with Leak Sanitizer..."
	$(CC) $(CFLAGS) -g -O0 -fsanitize=leak -fno-omit-frame-pointer -c -o $(BUILD_DIR)/logger_leak.o $(LOGGER_SRC)
	$(CC) $(CFLAGS) -g -O0 -fsanitize=leak -fno-omit-frame-pointer -c -o $(BUILD_DIR)/migrations_leak.o $(MIGRATIONS_SRC)
	$(CC) $(CFLAGS) -g -O0 -fsanitize=leak -fno-omit-frame-pointer -c -o $(BUILD_DIR)/persistence_leak.o $(PERSISTENCE_SRC)
	$(CC) $(CFLAGS) -g -O0 -fsanitize=leak -fno-omit-frame-pointer -o $(BUILD_DIR)/claude-c-lsan $(SRC) $(BUILD_DIR)/logger_leak.o $(BUILD_DIR)/persistence_leak.o $(BUILD_DIR)/migrations_leak.o $(LDFLAGS) -fsanitize=leak
	@echo ""
	@echo "✓ Build successful with LeakSanitizer!"
	@echo "Run: ./$(BUILD_DIR)/claude-c-lsan \"your prompt here\""
	@echo ""

# Run Valgrind memory checker on tests
valgrind: test-edit test-read
	@echo ""
	@echo "Running Valgrind on test suite..."
	@echo ""
	@command -v valgrind >/dev/null 2>&1 || { echo "Error: valgrind not found. Install with: brew install valgrind (macOS) or apt-get install valgrind (Linux)"; exit 1; }
	@echo "=== Testing Edit tool with Valgrind ==="
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 ./$(TEST_EDIT_TARGET)
	@echo ""
	@echo "=== Testing Read tool with Valgrind ==="
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 ./$(TEST_READ_TARGET)
	@echo ""
	@echo "✓ Valgrind checks complete - no memory leaks detected!"
	@echo ""

# Comprehensive memory bug scan - runs all analysis tools
memscan: analyze sanitize-all
	@echo ""
	@echo "=========================================="
	@echo "Comprehensive Memory Bug Scan Complete"
	@echo "=========================================="
	@echo ""
	@echo "Completed checks:"
	@echo "  ✓ Static analysis (see $(BUILD_DIR)/analyze.log)"
	@echo "  ✓ Built with combined sanitizers ($(BUILD_DIR)/claude-allsan)"
	@echo ""
	@echo "Next steps:"
	@echo "  1. Review static analysis results: cat $(BUILD_DIR)/analyze.log"
	@echo "  2. Test with sanitizers: ./$(BUILD_DIR)/claude-c-allsan \"test prompt\""
	@echo "  3. Run Valgrind: make valgrind"
	@echo ""
	@echo "For production testing, run all test suites with sanitizers:"
	@echo "  ./$(BUILD_DIR)/claude-c-allsan --help"
	@echo ""

$(LOGGER_OBJ): $(LOGGER_SRC) src/logger.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(LOGGER_OBJ) $(LOGGER_SRC)

$(PERSISTENCE_OBJ): $(PERSISTENCE_SRC) src/persistence.h src/migrations.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(PERSISTENCE_OBJ) $(PERSISTENCE_SRC)

$(MIGRATIONS_OBJ): $(MIGRATIONS_SRC) src/migrations.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(MIGRATIONS_OBJ) $(MIGRATIONS_SRC)

$(COMMANDS_OBJ): $(COMMANDS_SRC) src/commands.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(COMMANDS_OBJ) $(COMMANDS_SRC)

$(COMPLETION_OBJ): $(COMPLETION_SRC) src/completion.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(COMPLETION_OBJ) $(COMPLETION_SRC)

$(TUI_OBJ): $(TUI_SRC) src/tui.h src/claude_internal.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(TUI_OBJ) $(TUI_SRC)


$(HISTORY_FILE_OBJ): $(HISTORY_FILE_SRC) src/history_file.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(HISTORY_FILE_OBJ) $(HISTORY_FILE_SRC)


$(WINDOW_MANAGER_OBJ): $(WINDOW_MANAGER_SRC) src/window_manager.h src/logger.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(WINDOW_MANAGER_OBJ) $(WINDOW_MANAGER_SRC)
$(AI_WORKER_OBJ): $(AI_WORKER_SRC) src/ai_worker.h src/message_queue.h src/claude_internal.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(AI_WORKER_OBJ) $(AI_WORKER_SRC)

$(VOICE_INPUT_OBJ): $(VOICE_INPUT_SRC) src/voice_input.h src/logger.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(VOICE_INPUT_OBJ) $(VOICE_INPUT_SRC)

$(MCP_OBJ): $(MCP_SRC) src/mcp.h src/logger.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(MCP_OBJ) $(MCP_SRC)

$(TODO_OBJ): $(TODO_SRC) src/todo.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(TODO_OBJ) $(TODO_SRC)

$(AWS_BEDROCK_OBJ): $(AWS_BEDROCK_SRC) src/aws_bedrock.h src/logger.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(AWS_BEDROCK_OBJ) $(AWS_BEDROCK_SRC)

$(PROVIDER_OBJ): $(PROVIDER_SRC) src/provider.h src/openai_provider.h src/bedrock_provider.h src/logger.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(PROVIDER_OBJ) $(PROVIDER_SRC)

$(OPENAI_PROVIDER_OBJ): $(OPENAI_PROVIDER_SRC) src/openai_provider.h src/provider.h src/logger.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(OPENAI_PROVIDER_OBJ) $(OPENAI_PROVIDER_SRC)

$(OPENAI_MESSAGES_OBJ): $(OPENAI_MESSAGES_SRC) src/openai_messages.h src/claude_internal.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(OPENAI_MESSAGES_OBJ) $(OPENAI_MESSAGES_SRC)

$(BEDROCK_PROVIDER_OBJ): $(BEDROCK_PROVIDER_SRC) src/bedrock_provider.h src/provider.h src/aws_bedrock.h src/logger.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(BEDROCK_PROVIDER_OBJ) $(BEDROCK_PROVIDER_SRC)

$(BUILTIN_THEMES_OBJ): $(BUILTIN_THEMES_SRC) src/builtin_themes.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(BUILTIN_THEMES_OBJ) $(BUILTIN_THEMES_SRC)

$(PATCH_PARSER_OBJ): $(PATCH_PARSER_SRC) src/patch_parser.h src/claude_internal.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(PATCH_PARSER_OBJ) $(PATCH_PARSER_SRC)

$(MESSAGE_QUEUE_OBJ): $(MESSAGE_QUEUE_SRC) src/message_queue.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(MESSAGE_QUEUE_OBJ) $(MESSAGE_QUEUE_SRC)

$(TOOL_UTILS_OBJ): $(TOOL_UTILS_SRC) src/tool_utils.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(TOOL_UTILS_OBJ) $(TOOL_UTILS_SRC)

# Query tool - utility to inspect API call logs
$(QUERY_TOOL): $(QUERY_TOOL_SRC) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Building query tool..."
	@$(CC) $(CFLAGS) -o $(QUERY_TOOL) $(QUERY_TOOL_SRC) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) -lsqlite3
	@echo ""
	@echo "✓ Query tool built successfully!"
	@echo "Run: ./$(QUERY_TOOL) --help"
	@echo ""

# Test target for Window Manager - layout and pad capacity behavior
$(TEST_WM_TARGET): $(TEST_WM_SRC) $(WINDOW_MANAGER_OBJ) $(LOGGER_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling Window Manager tests..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_window_manager.o $(TEST_WM_SRC)
	@echo "Linking Window Manager test executable..."
	@$(CC) -o $(TEST_WM_TARGET) $(BUILD_DIR)/test_window_manager.o $(WINDOW_MANAGER_OBJ) $(LOGGER_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Window Manager test build successful!"
	@echo ""

# Test target for Edit tool - compiles test suite with claude.c functions
# We rename claude's main to avoid conflict with test's main
# and export internal functions via TEST_BUILD flag
$(TEST_EDIT_TARGET): $(SRC) $(TEST_EDIT_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for testing (renaming main)..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -c -o $(BUILD_DIR)/claude_test.o $(SRC)
	@echo "Compiling Edit tool test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_edit.o $(TEST_EDIT_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_EDIT_TARGET) $(BUILD_DIR)/claude_test.o $(BUILD_DIR)/test_edit.o $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Edit tool test build successful!"
	@echo ""

# Test target for Read tool - compiles test suite with claude.c functions
$(TEST_READ_TARGET): $(SRC) $(TEST_READ_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for read testing..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -c -o $(BUILD_DIR)/claude_read_test.o $(SRC)
	@echo "Compiling Read tool test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_read.o $(TEST_READ_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_READ_TARGET) $(BUILD_DIR)/claude_read_test.o $(BUILD_DIR)/test_read.o $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Read tool test build successful!"
	@echo ""


# Test target for TODO list - tests task management functionality
$(TEST_TODO_TARGET): $(TODO_SRC) $(TEST_TODO_SRC) tests/test_todo_stubs.c
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling TODO list test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/todo_test.o $(TODO_SRC)
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_todo.o $(TEST_TODO_SRC)
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_todo_stubs.o tests/test_todo_stubs.c
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_TODO_TARGET) $(BUILD_DIR)/todo_test.o $(BUILD_DIR)/test_todo.o $(BUILD_DIR)/test_todo_stubs.o $(LDFLAGS)
	@echo ""
	@echo "✓ TODO list test build successful!"
	@echo ""

# Test target for TodoWrite tool - tests integration with claude.c
$(TEST_TODO_WRITE_TARGET): $(SRC) $(TEST_TODO_WRITE_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for TodoWrite testing (renaming main)..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -c -o $(BUILD_DIR)/claude_todowrite_test.o $(SRC)
	@echo "Compiling TodoWrite tool test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_todo_write.o $(TEST_TODO_WRITE_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_TODO_WRITE_TARGET) $(BUILD_DIR)/claude_todowrite_test.o $(BUILD_DIR)/test_todo_write.o $(TODO_OBJ) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ TodoWrite tool test build successful!"
	@echo ""

# Test target for Paste Handler - tests paste detection and sanitization
$(TEST_PASTE_TARGET): $(TEST_PASTE_SRC)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling Paste Handler test suite..."
	@$(CC) $(CFLAGS) -o $(TEST_PASTE_TARGET) $(TEST_PASTE_SRC)
	@echo ""
	@echo "✓ Paste Handler test build successful!"
	@echo ""

# Test target for Retry Jitter - tests exponential backoff with jitter
$(TEST_RETRY_JITTER_TARGET): $(TEST_RETRY_JITTER_SRC)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling Retry Jitter test suite..."
	@$(CC) $(CFLAGS) -o $(TEST_RETRY_JITTER_TARGET) $(TEST_RETRY_JITTER_SRC) -lm
	@echo ""
	@echo "✓ Retry Jitter test build successful!"
	@echo ""

# Test target for tool timing - ensures no 60-second delays
$(TEST_TIMING_TARGET): tests/test_tool_timing.c
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling tool timing test suite..."
	@$(CC) $(CFLAGS) -o $(TEST_TIMING_TARGET) tests/test_tool_timing.c -lpthread
	@echo ""
	@echo "✓ Tool timing test build successful!"
	@echo ""

# Test target for OpenAI message format validation
$(TEST_OPENAI_FORMAT_TARGET): $(TEST_OPENAI_FORMAT_SRC)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling OpenAI format validation test suite..."
	@$(CC) $(CFLAGS) -o $(TEST_OPENAI_FORMAT_TARGET) $(TEST_OPENAI_FORMAT_SRC) $(LDFLAGS)
	@echo ""
	@echo "✓ OpenAI format test build successful!"
	@echo ""

# Test target for cancel flow -> tool_result formatting
$(TEST_CANCEL_FLOW_TARGET): $(SRC) tests/test_cancel_flow.c $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for cancel flow testing..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -c -o $(BUILD_DIR)/claude_cancel_flow_test.o $(SRC)
	@echo "Compiling cancel flow test suite..."
	@$(CC) $(CFLAGS) -I./src -c -o $(BUILD_DIR)/test_cancel_flow.o tests/test_cancel_flow.c
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_CANCEL_FLOW_TARGET) $(BUILD_DIR)/claude_cancel_flow_test.o $(BUILD_DIR)/test_cancel_flow.o $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Cancel flow test build successful!"
	@echo ""

test-cancel-flow: check-deps $(TEST_CANCEL_FLOW_TARGET)
	@echo ""
	@echo "Running cancel flow tests..."
	@echo ""
	@./$(TEST_CANCEL_FLOW_TARGET)

# Test target for Write tool diff integration
$(TEST_WRITE_DIFF_INTEGRATION_TARGET): $(SRC) $(TEST_WRITE_DIFF_INTEGRATION_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for write diff testing..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -c -o $(BUILD_DIR)/claude_write_diff_test.o $(SRC)
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/tool_utils_test.o $(TOOL_UTILS_SRC)
	@echo "Compiling Write tool diff integration test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_write_diff_integration.o $(TEST_WRITE_DIFF_INTEGRATION_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_WRITE_DIFF_INTEGRATION_TARGET) $(BUILD_DIR)/claude_write_diff_test.o $(BUILD_DIR)/tool_utils_test.o $(BUILD_DIR)/test_write_diff_integration.o $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Write tool diff integration test build successful!"
	@echo ""

# Test target for database rotation
$(TEST_ROTATION_TARGET): $(TEST_ROTATION_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling database rotation tests..."
	$(CC) $(CFLAGS) -o $(TEST_ROTATION_TARGET) $(TEST_ROTATION_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Rotation test build successful!"
	@echo ""

# Test target for patch parser
$(TEST_PATCH_PARSER_TARGET): $(SRC) $(TEST_PATCH_PARSER_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for patch parser testing..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -c -o $(BUILD_DIR)/claude_patch_test.o $(SRC)
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/tool_utils_patch_test.o $(TOOL_UTILS_SRC)
	@echo "Compiling Patch Parser test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_patch_parser.o $(TEST_PATCH_PARSER_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_PATCH_PARSER_TARGET) $(BUILD_DIR)/claude_patch_test.o $(BUILD_DIR)/tool_utils_patch_test.o $(BUILD_DIR)/test_patch_parser.o $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(PATCH_PARSER_OBJ) $(MESSAGE_QUEUE_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Patch Parser test build successful!"
	@echo ""

# Test target for thread cancellation
$(TEST_THREAD_CANCEL_TARGET): $(TEST_THREAD_CANCEL_SRC)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling Thread Cancellation test suite..."
	@$(CC) $(CFLAGS) -o $(TEST_THREAD_CANCEL_TARGET) $(TEST_THREAD_CANCEL_SRC) -lpthread
	@echo ""
	@echo "✓ Thread Cancellation test build successful!"
	@echo ""

# Test target for AWS credential rotation with polling
$(TEST_AWS_CRED_ROTATION_TARGET): $(TEST_AWS_CRED_ROTATION_SRC) $(AWS_BEDROCK_OBJ) $(LOGGER_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling AWS Credential Rotation test suite..."
	@$(CC) $(CFLAGS) -o $(TEST_AWS_CRED_ROTATION_TARGET) $(TEST_AWS_CRED_ROTATION_SRC) $(AWS_BEDROCK_OBJ) $(LOGGER_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ AWS Credential Rotation test build successful!"
	@echo ""

# Test target for message queues - tests thread-safe queues
$(TEST_MESSAGE_QUEUE_TARGET): $(TEST_MESSAGE_QUEUE_SRC) $(MESSAGE_QUEUE_OBJ) $(LOGGER_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling Message Queue test suite..."
	@$(CC) $(CFLAGS) -o $(TEST_MESSAGE_QUEUE_TARGET) $(TEST_MESSAGE_QUEUE_SRC) $(MESSAGE_QUEUE_OBJ) $(LOGGER_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Message Queue test build successful!"
	@echo ""

$(TEST_EVENT_LOOP_TARGET): $(TEST_EVENT_LOOP_SRC) $(TEST_STUBS_SRC) $(TUI_OBJ) $(WINDOW_MANAGER_OBJ) $(MESSAGE_QUEUE_OBJ) $(LOGGER_OBJ) $(TODO_OBJ) $(BUILTIN_THEMES_OBJ) $(PATCH_PARSER_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling Event Loop test..."
	@$(CC) $(CFLAGS) -Wno-unused-function -o $(TEST_EVENT_LOOP_TARGET) $(TEST_EVENT_LOOP_SRC) $(TEST_STUBS_SRC) $(TUI_OBJ) $(MESSAGE_QUEUE_OBJ) $(LOGGER_OBJ) $(TODO_OBJ) $(BUILTIN_THEMES_OBJ) $(PATCH_PARSER_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Event Loop test build successful!"
	@echo ""

$(TEST_TEXT_WRAP_TARGET): tests/test_text_wrap.c
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling Text Wrapping test..."
	@$(CC) -Wall -Wextra -O0 -g -o $(TEST_TEXT_WRAP_TARGET) tests/test_text_wrap.c -I./src
	@echo ""
	@echo "✓ Text Wrapping test build successful!"
	@echo ""

$(TEST_MCP_TARGET): $(TEST_MCP_SRC) $(MCP_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling MCP integration tests..."
	@$(CC) $(CFLAGS) -o $(TEST_MCP_TARGET) $(TEST_MCP_SRC) $(MCP_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ MCP test build successful!"
	@echo ""

install: $(TARGET)
	@echo "Installing claude-c to $(INSTALL_PREFIX)/bin..."
	@mkdir -p $(INSTALL_PREFIX)/bin
	@cp $(TARGET) $(INSTALL_PREFIX)/bin/claude-c
ifeq ($(UNAME_S),Darwin)
ifndef GITHUB_ACTIONS
	@echo "Signing binary for macOS..."
	@codesign --force --deep --sign - $(INSTALL_PREFIX)/bin/claude-c 2>/dev/null || echo "Warning: Code signing failed (non-fatal)"
endif
endif
	@echo "✓ Installation complete! Run 'claude-c' from anywhere."
	@echo ""
	@echo "Note: Make sure $(INSTALL_PREFIX)/bin is in your PATH:"
	@echo "  export PATH=\"$(INSTALL_PREFIX)/bin:$$PATH\""

clean:
	rm -rf $(BUILD_DIR)

# Format: Remove trailing whitespaces from source files
fmt-whitespace:
	@echo "Removing trailing whitespaces from source files..."
	@find src tests tools -type f \( -name "*.c" -o -name "*.h" \) -exec sed -i '' 's/[[:space:]]*$$//' {} +
	@echo "✓ Trailing whitespaces removed"

check-deps:
	@echo "Checking dependencies..."
	@command -v $(CC) >/dev/null 2>&1 || { echo "Error: gcc not found. Please install gcc."; exit 1; }
	@command -v curl-config >/dev/null 2>&1 || { echo "Error: libcurl not found. Install with: brew install curl (macOS) or apt-get install libcurl4-openssl-dev (Linux)"; exit 1; }
	@command -v pkg-config >/dev/null 2>&1 || { echo "Warning: pkg-config not found. May have issues detecting OpenSSL."; }
	@pkg-config --exists openssl 2>/dev/null || { echo "Error: OpenSSL not found. Install with: brew install openssl (macOS) or apt-get install libssl-dev (Linux)"; exit 1; }
	@echo "✓ All dependencies found"
	@echo ""

help:
	@echo "Claude Code - Pure C Edition - Build System"
	@echo ""
	@echo "Usage:"
	@echo "  make           - Build the claude-c executable"
	@echo "  make clang     - Build with clang compiler"
	@echo "  make debug     - Build with AddressSanitizer (memory bug detection)"
	@echo "  make test      - Build and run all unit tests"
	@echo "  make test-edit - Build and run Edit tool tests only"
	@echo "  make test-read - Build and run Read tool tests only"
	@echo "  make test-todo - Build and run TODO list tests only"
	@echo "  make test-paste - Build and run Paste Handler tests only"
	@echo "  make test-retry-jitter - Build and run Retry Jitter tests only"
	@echo "  make test-message-queue - Build and run Message Queue tests only"
	@echo "  make query-tool - Build the API call log query utility"
	@echo "  make clean     - Remove built files"
	@echo "  make install   - Install to \$$HOME/.local/bin as claude-c (default)"
	@echo "  make install INSTALL_PREFIX=/usr/local - Install to /usr/local/bin (requires sudo)"
	@echo "  make install INSTALL_PREFIX=/opt - Install to /opt/bin (requires sudo)"
	@echo "  make check-deps - Check if all dependencies are installed"
	@echo ""
	@echo "CI Testing (replicate GitHub Actions locally):"
	@echo "  make ci-test        - Quick CI check (GCC + Clang with sanitizers)"
	@echo "  make ci-all         - Full CI matrix (all compiler/sanitizer combos)"
	@echo "  make ci-gcc         - Test with GCC only"
	@echo "  make ci-clang       - Test with Clang only"
	@echo "  make ci-gcc-sanitize - Test with GCC + sanitizers"
	@echo "  make ci-clang-sanitize - Test with Clang + sanitizers"
	@echo ""
	@echo "Version Management:"
	@echo "  make version        - Show current version"
	@echo "  make show-version   - Show detailed version information"
	@echo "  make bump-patch     - Increment patch version and update README (e.g., 0.0.2 → 0.0.3)"
	@echo "  make update-version VERSION=1.0.0 - Set specific version number"
	@echo ""
	@echo "Memory Bug Scanning:"
	@echo "  make memscan      - Run comprehensive memory analysis (recommended)"
	@echo "  make analyze      - Run static analysis (clang/gcc analyzer)"
	@echo "  make sanitize-all - Build with Address + UB sanitizers (recommended)"
	@echo "  make sanitize-ub  - Build with Undefined Behavior Sanitizer only"
	@echo "  make sanitize-leak - Build with Leak Sanitizer only"
	@echo "  make valgrind     - Run test suite under Valgrind"
	@echo ""
	@echo "Code Formatting:"
	@echo "  make fmt-whitespace - Remove trailing whitespaces from all source files"
	@echo ""
	@echo "Dependencies:"
	@echo "  - gcc or clang (or compatible C compiler)"
	@echo "  - libcurl"
	@echo "  - cJSON"
	@echo "  - sqlite3"
	@echo "  - OpenSSL (for AWS Bedrock support)"
	@echo "  - pthread (usually included with OS)"
	@echo "  - valgrind (optional, for memory leak detection)"
	@echo ""
	@echo "macOS installation:"
	@echo "  brew install curl cjson sqlite3 openssl valgrind"
	@echo ""
	@echo "Linux installation:"
	@echo "  apt-get install libcurl4-openssl-dev libcjson-dev libsqlite3-dev libssl-dev valgrind"
	@echo "  or"
	@echo "  yum install libcurl-devel cjson-devel sqlite-devel openssl-devel valgrind"
	@echo ""
	@echo "AWS Bedrock Configuration:"
	@echo "  export CLAUDE_CODE_USE_BEDROCK=true"
	@echo "  export ANTHROPIC_MODEL=us.anthropic.claude-sonnet-4-5-20250929-v1:0"
	@echo "  export AWS_REGION=us-west-2"
	@echo "  export AWS_PROFILE=your-profile"
	@echo ""
	@echo "Optional - Custom Authentication:"
	@echo "  export AWS_AUTH_COMMAND='cd /path/to/okta && bash script.sh'"

# Version management targets
version:
	@echo "$(VERSION)"

show-version: $(VERSION_H)
	@echo "Claude C - Pure C Edition"
	@echo "Version: $(VERSION)"
	@echo "Build date: $(BUILD_DATE)"
	@echo "Version file: $(VERSION_FILE)"
	@echo "Version header: $(VERSION_H)"
	@if [ -f "$(TARGET)" ]; then \
		echo "Binary: $(TARGET)"; \
		./$(TARGET) --version 2>/dev/null || echo "Binary version: not available"; \
	fi

update-version:
	@if [ -z "$(VERSION)" ]; then \
		echo "Error: VERSION parameter required"; \
		echo "Usage: make update-version VERSION=1.0.0"; \
		exit 1; \
	fi
	@echo "Updating version to $(VERSION)..."
	@echo "$(VERSION)" > $(VERSION_FILE)
	@echo "✓ Updated $(VERSION_FILE) to $(VERSION)"
	@echo "Run 'make clean && make' to rebuild with new version"

bump-patch:
	@echo "Current version: $(VERSION)"
	@CURRENT_VERSION="$(VERSION)"; \
	VERSION_MAJOR=$$(echo "$$CURRENT_VERSION" | sed 's/^\([0-9]*\)\..*/\1/'); \
	VERSION_MINOR=$$(echo "$$CURRENT_VERSION" | sed 's/^[0-9]*\.\([0-9]*\)\..*/\1/'); \
	VERSION_PATCH=$$(echo "$$CURRENT_VERSION" | sed 's/^[0-9]*\.[0-9]*\.\([0-9]*\).*/\1/'); \
	NEW_PATCH=$$((VERSION_PATCH + 1)); \
	NEW_VERSION="$$VERSION_MAJOR.$$VERSION_MINOR.$$NEW_PATCH"; \
	echo "Bumping patch version: $$CURRENT_VERSION → $$NEW_VERSION"; \
	echo "$$NEW_VERSION" > $(VERSION_FILE); \
	echo "✓ Version bumped to $$NEW_VERSION"; \
	echo ""; \
	echo "Regenerating version.h..."; \
	rm -f $(VERSION_H); \
	$(MAKE) $(VERSION_H); \
	echo ""; \
	echo "Updating README.md with new version..."; \
	sed -i.bak 's/git clone --branch v[0-9]*\.[0-9]*\.[0-9]*/git clone --branch v'"$$NEW_VERSION"'/g' README.md; \
	rm -f README.md.bak; \
	echo "✓ Updated README.md with v$$NEW_VERSION"; \
	echo ""; \
	echo "Staging version files..."; \
	git add $(VERSION_FILE) $(VERSION_H) README.md; \
	git commit -m "chore: bump version to $$NEW_VERSION"; \
	echo ""; \
	echo "Creating git tag v$$NEW_VERSION..."; \
	git tag -a "v$$NEW_VERSION" -m "Release v$$NEW_VERSION"; \
	echo ""; \
	echo "Pushing to remote..."; \
	git push origin master; \
	git push origin "v$$NEW_VERSION"; \
	echo ""; \
	echo "✓ Version $$NEW_VERSION released successfully!"; \
	echo "  - Committed: $(VERSION_FILE) and $(VERSION_H)"; \
	echo "  - Updated: README.md with new version"; \
	echo "  - Tagged: v$$NEW_VERSION"; \
	echo "  - Pushed to remote"

# CI-like testing targets - mirror what GitHub Actions does
ci-gcc: check-deps
	@echo "=========================================="
	@echo "CI Test: Building with GCC"
	@echo "=========================================="
	@echo ""
	@$(MAKE) clean
	@CC=gcc $(MAKE) all
	@CC=gcc $(MAKE) test
	@echo ""
	@echo "✓ GCC build and tests passed!"
	@echo ""

ci-clang: check-deps
	@echo "=========================================="
	@echo "CI Test: Building with Clang"
	@echo "=========================================="
	@echo ""
	@$(MAKE) clean
	@CC=clang $(MAKE) all
	@CC=clang $(MAKE) test
	@echo ""
	@echo "✓ Clang build and tests passed!"
	@echo ""

ci-gcc-sanitize: check-deps
	@echo "=========================================="
	@echo "CI Test: Building with GCC + Sanitizers"
	@echo "=========================================="
	@echo ""
	@$(MAKE) clean
	@CC=gcc SANITIZERS=-fsanitize=address,undefined $(MAKE) all
	@CC=gcc SANITIZERS=-fsanitize=address,undefined $(MAKE) test
	@echo ""
	@echo "✓ GCC + sanitizers build and tests passed!"
	@echo ""

ci-clang-sanitize: check-deps
	@echo "=========================================="
	@echo "CI Test: Building with Clang + Sanitizers"
	@echo "=========================================="
	@echo ""
	@$(MAKE) clean
	@CC=clang SANITIZERS=-fsanitize=address,undefined $(MAKE) all
	@CC=clang SANITIZERS=-fsanitize=address,undefined $(MAKE) test
	@echo ""
	@echo "✓ Clang + sanitizers build and tests passed!"
	@echo ""

# Run all CI test combinations (what GitHub Actions does)
ci-all: check-deps
	@echo "=========================================="
	@echo "Running Complete CI Test Matrix"
	@echo "=========================================="
	@echo ""
	@echo "This will test all compiler+sanitizer combinations like GitHub Actions"
	@echo ""
	@$(MAKE) ci-gcc
	@$(MAKE) ci-clang
	@$(MAKE) ci-gcc-sanitize
	@$(MAKE) ci-clang-sanitize
	@echo ""
	@echo "=========================================="
	@echo "✓ ALL CI TESTS PASSED!"
	@echo "=========================================="
	@echo ""
	@echo "Tested combinations:"
	@echo "  ✓ GCC build + tests"
	@echo "  ✓ Clang build + tests"
	@echo "  ✓ GCC + sanitizers build + tests"
	@echo "  ✓ Clang + sanitizers build + tests"
	@echo ""
	@echo "Your code should pass CI!"
	@echo ""

# Quick CI check - just the most important combinations
ci-test: ci-gcc ci-clang-sanitize
	@echo ""
	@echo "=========================================="
	@echo "✓ Quick CI Check Complete"
	@echo "=========================================="
	@echo ""
	@echo "Tested:"
	@echo "  ✓ GCC build + tests (default CI compiler)"
	@echo "  ✓ Clang + sanitizers (catches most issues)"
	@echo ""
	@echo "For full CI coverage, run: make ci-all"
	@echo ""
# Test target for Bash command summarization
# Note: test_bash_summary.c file does not exist, so test-bash-summary target is removed
