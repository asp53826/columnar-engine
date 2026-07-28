CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS ?=
CPPFLAGS += -Iinclude

ifeq ($(shell uname -s),Darwin)
MACOS_SDK := $(shell xcrun --show-sdk-path)
CPPFLAGS += -isysroot $(MACOS_SDK) -isystem $(MACOS_SDK)/usr/include/c++/v1
LDFLAGS += -isysroot $(MACOS_SDK)
endif

BUILD_DIR := build
OBJECT := $(BUILD_DIR)/engine.o
TOOL := $(BUILD_DIR)/columnar_tool
TEST := $(BUILD_DIR)/test_engine

.PHONY: all test benchmark clean

all: $(TOOL)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJECT): src/engine.cpp include/columnar/engine.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TOOL): src/main.cpp $(OBJECT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(TEST): tests/test_engine.cpp $(OBJECT)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

test: $(TEST)
	./$(TEST)

benchmark: $(TOOL)
	./$(TOOL) benchmark

clean:
	rm -rf $(BUILD_DIR)
