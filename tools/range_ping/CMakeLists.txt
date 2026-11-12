cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(range_ping LANGUAGES C CXX)

# The frame format is shared with tools/range_pong.
target_include_directories(app PRIVATE ../range_common)
target_sources(app PRIVATE
  src/main.cpp
)
