CXX      ?= g++
OPENSSL  ?= $(shell brew --prefix openssl@3 2>/dev/null)
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -O0 -g -Isrc -Itests -I$(OPENSSL)/include
LDFLAGS  ?= -L$(OPENSSL)/lib -lcrypto

HOST_SRCS = src/mfs_protocol.cpp src/access_keys.cpp tests/crypto_openssl.cpp
TEST_SRCS = $(wildcard tests/test_*.cpp)
SRCS      = $(HOST_SRCS) $(TEST_SRCS)

test: $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) -o test_runner $(LDFLAGS)
	./test_runner

clean:
	rm -rf test_runner test_runner.dSYM

.PHONY: test clean
