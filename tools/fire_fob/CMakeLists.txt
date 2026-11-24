cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fire_fob LANGUAGES C CXX)

# The key header is generated per day by gen_fob_keys.py into the gitignored secrets/.
# FOB_KEYS_DIR overrides the directory (gen_fob_keys.py --fake --out <dir>, for a compile check).
if(NOT DEFINED FOB_KEYS_DIR)
  set(FOB_KEYS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../secrets)
endif()
if(NOT EXISTS ${FOB_KEYS_DIR}/fire_fob_keys.h)
  message(FATAL_ERROR "${FOB_KEYS_DIR}/fire_fob_keys.h missing - run tools/fire_fob/gen_fob_keys.py first (README.md).")
endif()
target_include_directories(app PRIVATE ${FOB_KEYS_DIR})
target_sources(app PRIVATE src/main.cpp)
