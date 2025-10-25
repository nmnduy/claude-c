# Makefile for Claude Code - Pure C Edition

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lcurl -lpthread -lsqlite3

# Detect OS for cJSON library linking
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    # macOS - check for Homebrew installation
    HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null)
    ifneq ($(HOMEBREW_PREFIX),)
        CFLAGS += -I$(HOMEBREW_PREFIX)/include
        LDFLAGS += -L$(HOMEBREW_PREFIX)/lib
    endif
    LDFLAGS += -lcjson
else ifeq ($(UNAME_S),Linux)
    # Linux
    LDFLAGS += -lcjson
endif

BUILD_DIR = build
TARGET = $(BUILD_DIR)/claude
TEST_EDIT_TARGET = $(BUILD_DIR)/test_edit
TEST_INPUT_TARGET = $(BUILD_DIR)/test_input
QUERY_TOOL = $(BUILD_DIR)/query_logs
SRC = src/claude.c
LOGGER_SRC = src/logger.c
LOGGER_OBJ = $(BUILD_DIR)/logger.o
PERSISTENCE_SRC = src/persistence.c
PERSISTENCE_OBJ = $(BUILD_DIR)/persistence.o
MIGRATIONS_SRC = src/migrations.c
MIGRATIONS_OBJ = $(BUILD_DIR)/migrations.o
TEST_EDIT_SRC = tests/test_edit.c
TEST_INPUT_SRC = tests/test_input.c
QUERY_TOOL_SRC = tools/query_logs.c

.PHONY: all clean install check-deps test test-edit test-input query-tool

all: check-deps $(TARGET)

query-tool: check-deps $(QUERY_TOOL)

test: test-edit test-input

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

$(TARGET): $(SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Build successful!"
	@echo "Run: ./$(TARGET) \"your prompt here\""
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
$(TEST_EDIT_TARGET): $(SRC) $(TEST_EDIT_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for testing (renaming main)..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -Dmain=unused_main -c -o $(BUILD_DIR)/claude_test.o $(SRC)
	@echo "Compiling Edit tool test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_edit.o $(TEST_EDIT_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_EDIT_TARGET) $(BUILD_DIR)/claude_test.o $(BUILD_DIR)/test_edit.o $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Edit tool test build successful!"
	@echo ""

# Test target for Input handler - compiles test suite with claude.c functions
$(TEST_INPUT_TARGET): $(SRC) $(TEST_INPUT_SRC) $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ)
	@mkdir -p $(BUILD_DIR)
	@echo "Compiling claude.c for input testing..."
	@$(CC) $(CFLAGS) -DTEST_BUILD -Dmain=unused_main -c -o $(BUILD_DIR)/claude_input_test.o $(SRC)
	@echo "Compiling Input handler test suite..."
	@$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/test_input.o $(TEST_INPUT_SRC)
	@echo "Linking test executable..."
	@$(CC) -o $(TEST_INPUT_TARGET) $(BUILD_DIR)/claude_input_test.o $(BUILD_DIR)/test_input.o $(LOGGER_OBJ) $(PERSISTENCE_OBJ) $(MIGRATIONS_OBJ) $(LDFLAGS)
	@echo ""
	@echo "✓ Input handler test build successful!"
	@echo ""

clean:
	rm -rf $(BUILD_DIR)

install: $(TARGET)
	@echo "Installing $(TARGET) to /usr/local/bin..."
	@sudo cp $(TARGET) /usr/local/bin/
	@echo "✓ Installed successfully!"
	@echo "You can now run 'claude' from anywhere"

check-deps:
	@echo "Checking dependencies..."
	@command -v $(CC) >/dev/null 2>&1 || { echo "Error: gcc not found. Please install gcc."; exit 1; }
	@command -v curl-config >/dev/null 2>&1 || { echo "Error: libcurl not found. Install with: brew install curl (macOS) or apt-get install libcurl4-openssl-dev (Linux)"; exit 1; }
	@echo "✓ All dependencies found"
	@echo ""

help:
	@echo "Claude Code - Pure C Edition - Build System"
	@echo ""
	@echo "Usage:"
	@echo "  make           - Build the claude executable"
	@echo "  make test      - Build and run all unit tests"
	@echo "  make test-edit - Build and run Edit tool tests only"
	@echo "  make test-input - Build and run Input handler tests only"
	@echo "  make query-tool - Build the API call log query utility"
	@echo "  make clean     - Remove built files"
	@echo "  make install   - Install to /usr/local/bin (requires sudo)"
	@echo "  make check-deps - Check if all dependencies are installed"
	@echo ""
	@echo "Dependencies:"
	@echo "  - gcc (or compatible C compiler)"
	@echo "  - libcurl"
	@echo "  - cJSON"
	@echo "  - sqlite3"
	@echo "  - pthread (usually included with OS)"
	@echo ""
	@echo "macOS installation:"
	@echo "  brew install curl cjson sqlite3"
	@echo ""
	@echo "Linux installation:"
	@echo "  apt-get install libcurl4-openssl-dev libcjson-dev libsqlite3-dev"
	@echo "  or"
	@echo "  yum install libcurl-devel cjson-devel sqlite-devel"
