CXX ?= g++
CPPFLAGS := -Iinclude -Itests
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 -g
LDLIBS := -lcrypto -pthread

BUILD_DIR := build
CORE_SOURCES := $(wildcard src/*.cpp)
TEST_SOURCES := $(wildcard tests/*.cpp)
TEST_BINARY := $(BUILD_DIR)/swarmsync-tests
SWARMSYNC_BINARY := $(BUILD_DIR)/swarmsync
TRACKER_BINARY := $(BUILD_DIR)/swarmsync-tracker

.PHONY: all test clean

all: $(SWARMSYNC_BINARY) $(TRACKER_BINARY) $(TEST_BINARY)

test: $(SWARMSYNC_BINARY) $(TRACKER_BINARY) $(TEST_BINARY)
	TEST_FILTER="$(TEST_FILTER)" $(TEST_BINARY)
	bash tests/test_cli.sh
	bash tests/test_demo.sh

$(TEST_BINARY): $(CORE_SOURCES) $(TEST_SOURCES) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(SWARMSYNC_BINARY): $(CORE_SOURCES) apps/swarmsync_main.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(TRACKER_BINARY): $(CORE_SOURCES) apps/tracker_main.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
