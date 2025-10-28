# Makefile for Claude Code - Pure C Edition

CC ?= gcc
CFLAGS = -Wall -Wextra -Wpedantic -Wformat=2 -Wconversion -Wshadow -Wcast-qual -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wuninitialized -Warray-bounds -Wvla -Wwrite-strings -Wnull-dereference -Wimplicit-fallthrough -Wsign-conversion -Wsign-compare -Wfloat-equal -Wpointer-arith -Wbad-function-cast -Wstrict-overflow -Waggregate-return -Wredundant-decls -Wnested-externs -Winline -Wswitch-enum -Wswitch-default -Wenum-conversion -Wdisabled-optimization -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_BSD_SOURCE=1 $(SANITIZERS)
DEBUG_CFLAGS = -Wall -Wextra -Wpedantic -Wformat=2 -Wconversion -Wshadow -Wcast-qual -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wuninitialized -Warray-bounds -Wvla -Wwrite-strings -Wnull-dereference -Wimplicit-fallthrough -Wsign-conversion -Wsign-compare -Wfloat-equal -Wpointer-arith -Wbad-function-cast -Wstrict-overflow -Waggregate-return -Wredundant-decls -Wnested-externs -Winline -Wswitch-enum -Wswitch-default -Wenum-conversion -Wdisabled-optimization -g -O0 -std=c11 -D_POSIX_C_SOURCE=200809L -D_BSD_SOURCE=1 -fsanitize=address -fno-omit-frame-pointer
LDFLAGS = -lcurl -lpthread -lsqlite3 -lssl -lcrypto $(SANITIZERS)
DEBUG_LDFLAGS = -lcurl -lpthread -lsqlite3 -lssl -lcrypto -fsanitize=address

# Installation prefix (can be overridden via command line)
INSTALL_PREFIX ?= $(HOME)/.local

# Version management
VERSION_FILE := VERSION
VERSION := $(shell cat $(VERSION_FILE) 2>/dev/null || echo "unknown")
BUILD_DATE := $(shell date +%Y-%m-%d)
VERSION_H := src/version.h

# Detect OS for cJSON library linking
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS - check for Homebrew installation
    HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null)
    ifneq ($(HOMEBREW_PREFIX),)
        CFLAGS += -I$(HOMEBREW_PREFIX)/include
        DEBUG_CFLAGS += -I$(HOMEBREW_PREFIX)/include
        LDFLAGS += -L$(HOMEBREW_PREFIX)/lib
        DEBUG_LDFLAGS += -L$(HOMEBREW_PREFIX)/lib
    endif
    LDFLAGS += -lcjson
    DEBUG_LDFLAGS += -lcjson
else ifeq ($(UNAME_S),Linux)
    # Linux
    LDFLAGS += -lcjson
    DEBUG_LDFLAGS += -lcjson
endif

BUILD_DIR = build
TARGET = $(BUILD_DIR)/claude-c
TEST_EDIT_TARGET = $(BUILD_DIR)/test_edit
TEST_INPUT_TARGET = $(BUILD_DIR)/test_input
TEST_READ_TARGET = $(BUILD_DIR)/test_read
TEST_LINEEDIT_TARGET = $(BUILD_DIR)/test_lineedit
TEST_TODO_TARGET = $(BUILD_DIR)/test_todo
TEST_TODO_WRITE_TARGET = $(BUILD_DIR)/test_todo_write
TEST_TIMING_TARGET = $(BUILD_DIR)/test_tool_timing
TEST_PASTE_TARGET = $(BUILD_DIR)/test_paste
TEST_RETRY_JITTER_TARGET = $(BUILD_DIR)/test_retry_jitter
QUERY_TOOL = $(BUILD_DIR)/query_logs
SRC = src/claude.c
LOGGER_SRC = src/logger.c
LOGGER_OBJ = $(BUILD_DIR)/logger.o
PERSISTENCE_SRC = src/persistence.c
PERSISTENCE_OBJ = $(BUILD_DIR)/persistence.o
MIGRATIONS_SRC = src/migrations.c
MIGRATIONS_OBJ = $(BUILD_DIR)/migrations.o
LINEEDIT_SRC = src/lineedit.c
LINEEDIT_OBJ = $(BUILD_DIR)/lineedit.o
COMMANDS_SRC = src/commands.c
COMMANDS_OBJ = $(BUILD_DIR)/commands.o
COMPLETION_SRC = src/completion.c
COMPLETION_OBJ = $(BUILD_DIR)/completion.o
TUI_SRC = src/tui.c
TUI_OBJ = $(BUILD_DIR)/tui.o
TODO_SRC = src/todo.c
TODO_OBJ = $(BUILD_DIR)/todo.o
AWS_BEDROCK_SRC = src/aws_bedrock.c
AWS_BEDROCK_OBJ = $(BUILD_DIR)/aws_bedrock.o
TEST_EDIT_SRC = tests/test_edit.c
TEST_INPUT_SRC = tests/test_input.c
TEST_READ_SRC = tests/test_read.c
TEST_LINEEDIT_SRC = tests/test_lineedit.c
TEST_TODO_SRC = tests/test_todo.c
TEST_TODO_WRITE_SRC = tests/test_todo_write.c
TEST_PASTE_SRC = tests/test_paste.c
TEST_RETRY_JITTER_SRC = tests/test_retry_jitter.c
QUERY_TOOL_SRC = tools/query_logs.c

.PHONY: all clean check-deps install test test-edit test-input test-read test-lineedit test-todo test-todo-write test-paste test-retry-jitter query-tool debug analyze sanitize-ub sanitize-all sanitize-leak valgrind memscan version show-version update-version bump-patch build

all: check-deps $(TARGET)

build: check-deps $(TARGET)

debug: check-deps $(BUILD_DIR)/claude-c-debug

query-tool: check-deps $(QUERY_TOOL)

test: test-edit test-input test-read test-lineedit test-todo test-paste test-timing

test-edit: check-deps $(TEST_EDIT_TARGET)
	@echo ""
	@echo "Running Edit tool tests..."
	@echo ""
	@./$(TEST_EDIT_TARGET)

test-input: check-deps $(TEST_INPUT_TARGET)
	@echo ""
	@echo "Running Input handler tests..."
	@echo ""
	@./$(TEST_INPUT_TARGET)

test-read: check-deps $(TEST_READ_TARGET)
	@echo ""
	@echo "Running Read tool tests..."
	@echo ""
	@./$(TEST_READ_TARGET)

test-lineedit: check-deps $(TEST_LINEEDIT_TARGET)
	@echo ""
	@echo "Running Line Editor wrapping tests..."
	@echo ""
	@./$(TEST_LINEEDIT_TARGET)

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

$(TARGET): $(SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(LINEEDIT_OBJ) $(COMMANDS_OBJ) $(COMPLETION_OBJ) $(TUI_OBJ) $(TODO_OBJ) $(VERSION_H) $(AWS_BEDROCK_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(LINEEDIT_OBJ) $(COMMANDS_OBJ) $(COMPLETION_OBJ) $(TUI_OBJ) $(TODO_OBJ) $(AWS_BEDROCK_OBJ) $(LDFLAGS)
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
$(BUILD_DIR)/claude-c-debug: $(SRC) $(LOGGER_SRC) $(PERSISTENCE_SRC) $(MIGRATIONS_SRC) $(LINEEDIT_SRC) $(COMMANDS_SRC) $(COMPLETION_SRC) $(TUI_SRC) $(TODO_SRC) $(AWS_BEDROCK_SRC)
	@mkdir -p $(BUILD_DIR)
	@echo "Building with AddressSanitizer (debug mode)..."
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/logger_debug.o $(LOGGER_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/migrations_debug.o $(MIGRATIONS_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/persistence_debug.o $(PERSISTENCE_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/lineedit_debug.o $(LINEEDIT_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/commands_debug.o $(COMMANDS_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/completion_debug.o $(COMPLETION_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/tui_debug.o $(TUI_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/todo_debug.o $(TODO_SRC)
	$(CC) $(DEBUG_CFLAGS) -c -o $(BUILD_DIR)/aws_bedrock_debug.o $(AWS_BEDROCK_SRC)
	$(CC) $(DEBUG_CFLAGS) -o $(BUILD_DIR)/claude-c-debug $(SRC) $(BUILD_DIR)/logger_debug.o $(BUILD_DIR)/persistence_debug.o $(BUILD_DIR)/migrations_debug.o $(BUILD_DIR)/lineedit_debug.o $(BUILD_DIR)/commands_debug.o $(BUILD_DIR)/completion_debug.o $(BUILD_DIR)/tui_debug.o $(BUILD_DIR)/todo_debug.o $(BUILD_DIR)/aws_bedrock_debug.o $(DEBUG_LDFLAGS)
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
valgrind: test-edit test-input test-read
	@echo ""
	@echo "Running Valgrind on test suite..."
	@echo ""
	@command -v valgrind >/dev/null 2>&1 || { echo "Error: valgrind not found. Install with: brew install valgrind (macOS) or apt-get install valgrind (Linux)"; exit 1; }
	@echo "=== Testing Edit tool with Valgrind ==="
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 ./$(TEST_EDIT_TARGET)
	@echo ""
	@echo "=== Testing Input handler with Valgrind ==="
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 ./$(TEST_INPUT_TARGET)
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

$(LINEEDIT_OBJ): $(LINEEDIT_SRC) src/lineedit.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(LINEEDIT_OBJ) $(LINEEDIT_SRC)

$(COMMANDS_OBJ): $(COMMANDS_SRC) src/commands.h src/lineedit.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(COMMANDS_OBJ) $(COMMANDS_SRC)

$(COMPLETION_OBJ): $(COMPLETION_SRC) src/completion.h src/lineedit.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(COMPLETION_OBJ) $(COMPLETION_SRC)

$(TUI_OBJ): $(TUI_SRC) src/tui.h src/claude_internal.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(TUI_OBJ) $(TUI_SRC)

$(TODO_OBJ): $(TODO_SRC) src/todo.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(TODO_OBJ) $(TODO_SRC)

$(AWS_BEDROCK_OBJ): $(AWS_BEDROCK_SRC) src/aws_bedrock.h src/logger.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $(AWS_BEDROCK_OBJ) $(AWS_BEDROCK_SRC)

# Query tool - utility to inspect API call logs
$(QUERY_TOOL): $(QUERY_TOOL_SRC) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Building query tool..."
	@$(CC) $(CFLAGS) -o $(QUERY_TOOL) $(QUERY_TOOL_SRC) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) -lsqlite3
	@echo ""
	@echo "✓ Query tool built successfully!"
	@echo "Run: ./$(QUERY_TOOL) --help"
	@echo ""

# Test target for Edit tool - compiles test suite with claude.c functions
# We rename claude's main to avoid conflict with test's main
# and export internal functions via TEST_BUILD flag
$(TEST_EDIT_TARGET): $(SRC) $(TEST_EDIT_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for testing (renaming main)..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -Dmain=unused_main -c -o $(BUILD_DIR)/claude_test.o $(SRC)
	@echo "Compiling Edit tool test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_edit.o $(TEST_EDIT_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_EDIT_TARGET) $(BUILD_DIR)/claude_test.o $(BUILD_DIR)/test_edit.o $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Edit tool test build successful!"
	@echo ""

# Test target for Input handler - compiles test suite with lineedit.c functions
$(TEST_INPUT_TARGET): $(LINEEDIT_SRC) $(TEST_INPUT_SRC) $(LOGGER_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling lineedit.c for input testing..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -c -o $(BUILD_DIR)/lineedit_input_test.o $(LINEEDIT_SRC)
	@echo "Compiling Input handler test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_input.o $(TEST_INPUT_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_INPUT_TARGET) $(BUILD_DIR)/lineedit_input_test.o $(BUILD_DIR)/test_input.o $(LOGGER_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Input handler test build successful!"
	@echo ""

# Test target for Read tool - compiles test suite with claude.c functions
$(TEST_READ_TARGET): $(SRC) $(TEST_READ_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for read testing..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -Dmain=unused_main -c -o $(BUILD_DIR)/claude_read_test.o $(SRC)
	@echo "Compiling Read tool test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_read.o $(TEST_READ_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_READ_TARGET) $(BUILD_DIR)/claude_read_test.o $(BUILD_DIR)/test_read.o $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Read tool test build successful!"
	@echo ""

# Test target for Line Editor - tests wrapping calculation logic
$(TEST_LINEEDIT_TARGET): $(LINEEDIT_SRC) $(TEST_LINEEDIT_SRC) $(LOGGER_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling lineedit.c for testing..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -c -o $(BUILD_DIR)/lineedit_test.o $(LINEEDIT_SRC)
	@echo "Compiling Line Editor test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_lineedit.o $(TEST_LINEEDIT_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_LINEEDIT_TARGET) $(BUILD_DIR)/lineedit_test.o $(BUILD_DIR)/test_lineedit.o $(LOGGER_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Line Editor test build successful!"
	@echo ""

# Test target for TODO list - tests task management functionality
$(TEST_TODO_TARGET): $(TODO_SRC) $(TEST_TODO_SRC)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling TODO list test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/todo_test.o $(TODO_SRC)
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_todo.o $(TEST_TODO_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_TODO_TARGET) $(BUILD_DIR)/todo_test.o $(BUILD_DIR)/test_todo.o $(LDFLAGS)
	@echo ""
	@echo "✓ TODO list test build successful!"
	@echo ""

# Test target for TodoWrite tool - tests integration with claude.c
$(TEST_TODO_WRITE_TARGET): $(SRC) $(TEST_TODO_WRITE_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(TODO_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for TodoWrite testing (renaming main)..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -Dmain=unused_main -c -o $(BUILD_DIR)/claude_todowrite_test.o $(SRC)
	@echo "Compiling TodoWrite tool test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_todo_write.o $(TEST_TODO_WRITE_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_TODO_WRITE_TARGET) $(BUILD_DIR)/claude_todowrite_test.o $(BUILD_DIR)/test_todo_write.o $(TODO_OBJ) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(LDFLAGS)
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

install: $(TARGET)
	@echo "Installing claude-c to $(INSTALL_PREFIX)/bin..."
	@mkdir -p $(INSTALL_PREFIX)/bin
	@cp $(TARGET) $(INSTALL_PREFIX)/bin/claude-c
	@echo "✓ Installation complete! Run 'claude-c' from anywhere."
	@echo ""
	@echo "Note: Make sure $(INSTALL_PREFIX)/bin is in your PATH:"
	@echo "  export PATH=\"$(INSTALL_PREFIX)/bin:$$PATH\""

clean:
	rm -rf $(BUILD_DIR)

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
	@echo "  make debug     - Build with AddressSanitizer (memory bug detection)"
	@echo "  make test      - Build and run all unit tests"
	@echo "  make test-edit - Build and run Edit tool tests only"
	@echo "  make test-input - Build and run Input handler tests only"
	@echo "  make test-read - Build and run Read tool tests only"
	@echo "  make test-lineedit - Build and run Line Editor wrapping tests only"
	@echo "  make test-todo - Build and run TODO list tests only"
	@echo "  make test-paste - Build and run Paste Handler tests only"
	@echo "  make test-retry-jitter - Build and run Retry Jitter tests only"
	@echo "  make query-tool - Build the API call log query utility"
	@echo "  make clean     - Remove built files"
	@echo "  make install   - Install to \$$HOME/.local/bin as claude-c (default)"
	@echo "  make install INSTALL_PREFIX=/usr/local - Install to /usr/local/bin (requires sudo)"
	@echo "  make install INSTALL_PREFIX=/opt - Install to /opt/bin (requires sudo)"
	@echo "  make check-deps - Check if all dependencies are installed"
	@echo ""
	@echo "Version Management:"
	@echo "  make version        - Show current version"
	@echo "  make show-version   - Show detailed version information"
	@echo "  make bump-patch     - Increment patch version (e.g., 0.0.2 → 0.0.3)"
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
	@echo "Dependencies:"
	@echo "  - gcc (or compatible C compiler)"
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
	echo "Staging version files..."; \
	git add $(VERSION_FILE) $(VERSION_H); \
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
	echo "  - Tagged: v$$NEW_VERSION"; \
	echo "  - Pushed to remote"
