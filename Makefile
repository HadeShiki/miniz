# Author: HadeShiki
# License: MIT
#
#   make          # build demo + tests into build/
#   make test     # build and run tests
#   make run      # build and run demo
#   make clean    # remove build/

CXX      ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
SRC_DIR  := examples
INC_DIR  := include
BUILD_DIR:= build

DEMO_SRC  := $(SRC_DIR)/simple.cpp
TEST_SRC  := $(SRC_DIR)/test.cpp

TARGET_DEMO := $(BUILD_DIR)/miniz_demo
TARGET_TEST := $(BUILD_DIR)/miniz_test

.PHONY: all test run clean

all: $(TARGET_DEMO) $(TARGET_TEST)

# ---- build rules ----
$(TARGET_DEMO): $(DEMO_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) $< -o $@

$(TARGET_TEST): $(TEST_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) $< -o $@

# ---- order-only prerequisite: create build dir ----
ifeq ($(OS),Windows_NT)
$(BUILD_DIR):
	@if not exist "$(BUILD_DIR)\" mkdir "$(BUILD_DIR)"
else
$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)
endif

# ---- targets ----
test: $(TARGET_TEST)
	@./$(TARGET_TEST)

run: $(TARGET_DEMO)
	@./$(TARGET_DEMO)

clean:
ifeq ($(OS),Windows_NT)
	@if exist "$(BUILD_DIR)\" rmdir /s /q "$(BUILD_DIR)"
else
	@rm -rf $(BUILD_DIR)
endif