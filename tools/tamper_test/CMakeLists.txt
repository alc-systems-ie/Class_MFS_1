cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tamper_test LANGUAGES C CXX)

# The nPM2100 driver is shared with the main application, unmodified.
target_include_directories(app PRIVATE ../../src)
target_sources(app PRIVATE
  src/main.cpp
  ../../src/npm2100.cpp
  ../../src/npm2100_zephyr.cpp
)
