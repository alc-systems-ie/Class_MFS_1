CXX      ?= g++
OPENSSL  ?= $(shell brew --prefix openssl@3 2>/dev/null)
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -O0 -g -Isrc -Itests -I$(OPENSSL)/include
LDFLAGS  ?= -L$(OPENSSL)/lib -lcrypto

HOST_SRCS = src/mfs_protocol.cpp src/access_keys.cpp src/device_clock.cpp src/access_control.cpp src/led_sequencer.cpp src/settings.cpp src/credentials.cpp src/detection_engine.cpp src/arming_sequence.cpp tests/crypto_openssl.cpp
TEST_SRCS = $(wildcard tests/test_*.cpp)
SRCS      = $(HOST_SRCS) $(TEST_SRCS)

# Separate from CXXFLAGS so an override cannot drop it: it is what lets
# detection_engine.hpp take its host fallback instead of failing with #error.
HOST_DEFINES = -DALC_HOST_BUILD

test: $(SRCS)
	$(CXX) $(CXXFLAGS) $(HOST_DEFINES) $(SRCS) -o test_runner $(LDFLAGS)
	./test_runner

clean:
	rm -rf test_runner test_runner.dSYM

.PHONY: test clean
