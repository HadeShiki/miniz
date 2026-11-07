# miniz Makefile — cross-platform, header-only C++17
# Author: HadeShiki
# License: MIT
#
#   make          # build demo + tests into build/
#   make test     # build and run tests
#   make run      # build and run demo
#   make bench    # build and run benchmark (vs zlib/zstd)
#   make fuzz     # build and run fuzz test
#   make clean    # remove build/

CXX      ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
SRC_DIR  := examples
INC_DIR  := include
BUILD_DIR:= build

DEMO_SRC  := $(SRC_DIR)/simple.cpp
TEST_SRC  := $(SRC_DIR)/test.cpp
BENCH_SRC := $(SRC_DIR)/benchmark.cpp
FUZZ_SRC  := $(SRC_DIR)/fuzz.cpp

TARGET_DEMO := $(BUILD_DIR)/miniz_demo
TARGET_TEST := $(BUILD_DIR)/miniz_test
TARGET_BENCH:= $(BUILD_DIR)/miniz_bench
TARGET_FUZZ := $(BUILD_DIR)/miniz_fuzz

.PHONY: all test run bench fuzz clean

all: $(TARGET_DEMO) $(TARGET_TEST)

# ---- build rules ----
$(TARGET_DEMO): $(DEMO_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) $< -o $@

$(TARGET_TEST): $(TEST_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) $< -o $@

$(TARGET_BENCH): $(BENCH_SRC) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(INC_DIR) $< -lz -lzstd -o $@

$(TARGET_FUZZ): $(FUZZ_SRC) | $(BUILD_DIR)
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

bench: $(TARGET_BENCH)
	@./$(TARGET_BENCH)

fuzz: $(TARGET_FUZZ)
	@./$(TARGET_FUZZ)

clean:
ifneq ($(OS),Windows_NT)
	@rm -rf $(BUILD_DIR)
else
	@if exist "$(BUILD_DIR)\" rmdir /s /q "$(BUILD_DIR)"
endif