CXX ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pthread -Iinclude
LDFLAGS ?= -pthread

# Build configurations
DEBUG_FLAGS = -O0 -g -DDEBUG
RELEASE_FLAGS = -O3 -DNDEBUG
ASAN_FLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN_FLAGS = -O1 -g -fsanitize=thread

SRC_DIR = src
INC_DIR = include
TEST_DIR = tests
BIN_DIR = bin

COMMON_SRCS = $(SRC_DIR)/clock.cpp \
              $(SRC_DIR)/logger.cpp \
              $(SRC_DIR)/common.cpp \
              $(SRC_DIR)/ingest.cpp \
              $(SRC_DIR)/buffer.cpp \
              $(SRC_DIR)/memory_arena.cpp \
              $(SRC_DIR)/swap_engine.cpp \
              $(SRC_DIR)/disk_scan.cpp \
              $(SRC_DIR)/scheduler.cpp \
              $(SRC_DIR)/printer_pool.cpp \
              $(SRC_DIR)/telemetry.cpp

MAIN_SRC = $(SRC_DIR)/main.cpp

TARGET = print_spooler
TEST_TC01 = test_tc01
TEST_TC02 = test_tc02
TEST_TC03 = test_tc03
TEST_TC04 = test_tc04
TEST_TC05 = test_tc05

.PHONY: all release debug asan tsan test tc01 tc02 tc03 tc04 tc05 clean run

all: release

release: CXXFLAGS += $(RELEASE_FLAGS)
release: $(TARGET) $(TEST_TC01) $(TEST_TC02) $(TEST_TC03) $(TEST_TC04) $(TEST_TC05)

debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: $(TARGET) $(TEST_TC01) $(TEST_TC02) $(TEST_TC03) $(TEST_TC04) $(TEST_TC05)

asan: CXXFLAGS += $(ASAN_FLAGS)
asan: LDFLAGS += $(ASAN_FLAGS)
asan: $(TARGET) $(TEST_TC01) $(TEST_TC02) $(TEST_TC03) $(TEST_TC04) $(TEST_TC05)

tsan: CXXFLAGS += $(TSAN_FLAGS)
tsan: LDFLAGS += $(TSAN_FLAGS)
tsan: $(TARGET) $(TEST_TC01) $(TEST_TC02) $(TEST_TC03) $(TEST_TC04) $(TEST_TC05)

$(TARGET): $(COMMON_SRCS) $(MAIN_SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_TC01): $(COMMON_SRCS) $(TEST_DIR)/test_tc01_starvation.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_TC02): $(COMMON_SRCS) $(TEST_DIR)/test_tc02_stress.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_TC03): $(COMMON_SRCS) $(TEST_DIR)/test_tc03_arena_swap.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_TC04): $(COMMON_SRCS) $(TEST_DIR)/test_tc04_disk_scan.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_TC05): $(COMMON_SRCS) $(TEST_DIR)/test_tc05_swap_integrity.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

test: tc04 tc01 tc02 tc03 tc05
	@echo "All Test Cases Passed Successfully!"

tc01: $(TEST_TC01)
	./$(TEST_TC01)

tc02: $(TEST_TC02)
	./$(TEST_TC02)

tc03: $(TEST_TC03)
	./$(TEST_TC03)

tc04: $(TEST_TC04)
	./$(TEST_TC04)

tc05: $(TEST_TC05)
	./$(TEST_TC05)

run: $(TARGET)
	./$(TARGET) --workload workloads/sample_campus.txt

clean:
	rm -f $(TARGET) $(TARGET).exe $(TEST_TC01) $(TEST_TC01).exe $(TEST_TC02) $(TEST_TC02).exe $(TEST_TC03) $(TEST_TC03).exe $(TEST_TC04) $(TEST_TC04).exe $(TEST_TC05) $(TEST_TC05).exe *.o swap*.img gantt_timeline.csv
