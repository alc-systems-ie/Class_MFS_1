cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# The two long-term keys live in the KMU, provisioned by tools/flash_device.sh;
# the image must never carry them (design 2026-11-15 section 5.4). The Kconfig
# strings exist only for the host tools that read secrets/<serial>.conf.
#
# By the time this runs Kconfig has already written the secret into the build
# directory (.config, autoconf.h and, for a -D assignment, the extra-options file
# and the CLI_ cache entries), so all of those are removed before failing: a
# refused build must not leave a secret behind (design anti-goal).
set(mfs_secret_symbols_set "")
foreach(secret_symbol CONFIG_MFS_DEVICE_SECRET CONFIG_MFS_PROVISION_KEY)
  if(NOT "${${secret_symbol}}" STREQUAL "")
    list(APPEND mfs_secret_symbols_set ${secret_symbol})
  endif()
endforeach()
if(mfs_secret_symbols_set)
  file(REMOVE ${DOTCONFIG} ${DOTCONFIG}.old ${AUTOCONF_H} ${EXTRA_KCONFIG_OPTIONS_FILE})
  foreach(secret_symbol CONFIG_MFS_DEVICE_SECRET CONFIG_MFS_PROVISION_KEY)
    unset(${secret_symbol} CACHE)
    unset(CLI_${secret_symbol} CACHE)
    unset(${secret_symbol})
  endforeach()
  message(FATAL_ERROR "${mfs_secret_symbols_set} set: keys are provisioned into the KMU by tools/flash_device.sh "
                      "and must never be in a firmware build. The generated .config and autoconf.h were deleted. "
                      "Build with a conf holding only CONFIG_ALC_DEVICE_SERIAL and CONFIG_MFS_DEVICE_ID, or use tools/flash_device.sh.")
endif()

# Every image locks the debug port (prj.conf); on nRF54L the Kconfig default is
# unlocked, so a stray conf line could otherwise build an open image silently.
# The one sanctioned exception is debug-unlocked.conf, which sets
# MFS_DEBUG_UNLOCKED; the firmware warns at boot whenever either lock is off.
if(CONFIG_MFS_DEBUG_UNLOCKED)
  if(CONFIG_NRF_APPROTECT_LOCK OR CONFIG_NRF_SECURE_APPROTECT_LOCK)
    message(FATAL_ERROR "CONFIG_MFS_DEBUG_UNLOCKED is set but a debug-port lock is still selected: "
                        "use debug-unlocked.conf, which selects NRF_APPROTECT_DISABLE and NRF_SECURE_APPROTECT_DISABLE.")
  endif()
elseif(NOT (CONFIG_NRF_APPROTECT_LOCK AND CONFIG_NRF_SECURE_APPROTECT_LOCK))
  message(FATAL_ERROR "The debug port is not locked (CONFIG_NRF_APPROTECT_LOCK and CONFIG_NRF_SECURE_APPROTECT_LOCK "
                      "must both be y). Every image locks it; a bench build that needs RTT adds debug-unlocked.conf.")
endif()
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
  src/fire_gate.cpp
  src/key_check.cpp
  src/report_counter.cpp
  src/report_policy.cpp
  src/report_store.cpp
)
