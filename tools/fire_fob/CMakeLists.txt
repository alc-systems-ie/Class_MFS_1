cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fire_fob LANGUAGES C CXX)

# The key header is generated per day by gen_fob_keys.py into the gitignored secrets/.
set(FOB_KEYS ${CMAKE_CURRENT_SOURCE_DIR}/../../secrets/fire_fob_keys.h)
if(NOT EXISTS ${FOB_KEYS})
  message(FATAL_ERROR "secrets/fire_fob_keys.h missing - run tools/fire_fob/gen_fob_keys.py first (README.md).")
endif()
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../secrets)
target_sources(app PRIVATE src/main.cpp)
