cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# The two long-term keys live in the KMU, provisioned by tools/flash_device.sh;
# the image must never carry them (design 2026-11-15 section 5.4). The Kconfig
# strings exist only for the host tools that read secrets/<serial>.conf.
foreach(secret_symbol CONFIG_MFS_DEVICE_SECRET CONFIG_MFS_PROVISION_KEY)
  if(NOT "${${secret_symbol}}" STREQUAL "")
    message(FATAL_ERROR "${secret_symbol} is set: keys are provisioned into the KMU by tools/flash_device.sh "
                        "and must never be in a firmware build. Build with a conf holding only "
                        "CONFIG_ALC_DEVICE_SERIAL and CONFIG_MFS_DEVICE_ID, or use tools/flash_device.sh.")
  endif()
endforeach()
project(class_mfs_1 LANGUAGES C CXX)

target_sources(app PRIVATE
  src/main.cpp
  src/app.cpp
  src/adxl367.cpp
  src/command_scanner.cpp
  src/command_advertiser.cpp
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
  src/led_indicator.cpp
  src/detection_engine.cpp
  src/arming_sequence.cpp
  src/fire_sequence.cpp
  src/key_check.cpp
)
