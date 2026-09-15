cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(class_mfs_1 LANGUAGES C CXX)

target_sources(app PRIVATE
  src/main.cpp
  src/app.cpp
  src/adxl367.cpp
  src/command_scanner.cpp
  src/npm2100.cpp
  src/output_switch.cpp
  src/npm2100_zephyr.cpp
  src/mfs_protocol.cpp
  src/access_keys.cpp
  src/credentials.cpp
  src/crypto_psa.cpp
  src/crypto_selftest.cpp
  src/access_store.cpp
  src/access_control.cpp
  src/device_clock.cpp
  src/settings.cpp
  src/led_sequencer.cpp
  src/detection_engine.cpp
  src/arming_sequence.cpp
  src/fire_sequence.cpp
)
