CXX      ?= g++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -O0 -g -Isrc -Itests

HOST_SRCS =
TEST_SRCS = $(wildcard tests/test_*.cpp)
SRCS      = $(HOST_SRCS) $(TEST_SRCS)

test: $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o test_runner $(LDFLAGS)
	./test_runner

clean:
	rm -rf test_runner test_runner.dSYM

.PHONY: test clean
