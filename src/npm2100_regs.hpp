#pragma once
#include <cstdint>
#include <cstddef>

// Private register definitions for the nPM2100 PMIC.
// This header is included only by npm2100.cpp. Application code must not
// include it. All constants follow the M_<BLOCK>_<REGISTER>_<FIELD> convention.

namespace alc::npm2100::regs
{
  // ──────────────────── Common ────────────────────
  // Value written to a TASKS_* register to fire the task.
  constexpr uint8_t M_TASK_TRIGGER { 0x01 };
  // Maximum payload bytes for writeBurst (buffer is one bigger to hold reg addr).
  constexpr size_t M_WRITE_BURST_MAX_PAYLOAD { 15 };
  constexpr size_t M_WRITE_BURST_BUF_SIZE { M_WRITE_BURST_MAX_PAYLOAD + 1 };

  // ──────────────────── RESET (datasheet § 7.5.2) ────────────────────
  constexpr uint8_t M_RESET_TASKS_RESET { 0xD0 };
  constexpr uint8_t M_RESET_TASKS_CLR { 0xD1 };
  constexpr uint8_t M_RESET_RESET { 0xD5 };
  constexpr uint8_t M_RESET_SYSGDENSTATUS { 0xE2 };

  // RESET.RESET fields (read at Init time)
  constexpr uint8_t M_RESET_RESET_BOR_BIT { 0x01 };
  constexpr uint8_t M_RESET_RESET_REASON_MASK { 0x1E }; // bits[4:1]
  constexpr uint8_t M_RESET_RESET_REASON_POS { 1 };

  // RESET (continued)
  constexpr uint8_t M_RESET_BUTTON { 0xD2 };
  constexpr uint8_t M_RESET_PIN { 0xD3 };
  constexpr uint8_t M_RESET_DEBOUNCE { 0xD4 };
  constexpr uint8_t M_RESET_ALTCONFIG { 0xD6 };
  constexpr uint8_t M_RESET_WRITE { 0xD7 };
  constexpr uint8_t M_RESET_STROBE { 0xD8 };
  constexpr uint8_t M_RESET_READ { 0xD9 };
  constexpr uint8_t M_RESET_SCRATCHB { 0xDA };
  constexpr uint8_t M_RESET_WRITESTICKY { 0xDB };
  constexpr uint8_t M_RESET_STROBSTICKY { 0xDC };
  constexpr uint8_t M_RESET_READSTICKY { 0xDD };

  constexpr uint8_t M_RESET_BUTTON_LONGPRESS_BIT { 0x01 }; // 0=enable, 1=disable
  constexpr uint8_t M_RESET_PIN_SHPHLD_BIT { 0x01 };       // 0=PG/RESET, 1=SHPHLD
  constexpr uint8_t M_RESET_DEBOUNCE_TIME_MASK { 0x03 };
  constexpr uint8_t M_RESET_ALTCONFIG_LDOSW_BIT { 0x01 }; // 0=on, 1=off

  constexpr uint8_t M_RESET_STICKY_BOOTMONSEL_BIT { 1u << 0 };
  constexpr uint8_t M_RESET_STICKY_BOOTMONEN_BIT { 1u << 1 };
  constexpr uint8_t M_RESET_STICKY_PWRBUTTON_BIT { 1u << 2 };

  constexpr uint8_t M_RESET_SYSGDENSTATUS_BOOTMON_BIT { 1u << 0 };

  // ──────────────────── BOOST (datasheet § 6.1.6) ────────────────────
  constexpr uint8_t M_BOOST_TASKS_START { 0x20 };
  constexpr uint8_t M_BOOST_VOUT { 0x22 };
  constexpr uint8_t M_BOOST_VOUTSEL { 0x23 };
  constexpr uint8_t M_BOOST_OPER { 0x24 };
  constexpr uint8_t M_BOOST_CTRLSET { 0x2A };
  constexpr uint8_t M_BOOST_CTRLCLR { 0x2B };
  constexpr uint8_t M_BOOST_VBATMINHSEL { 0x2E };
  constexpr uint8_t M_BOOST_VBATMINH { 0x30 };
  constexpr uint8_t M_BOOST_STATUS0 { 0x34 };
  constexpr uint8_t M_BOOST_STATUS1 { 0x35 };

  // BOOST.VOUT field
  constexpr uint8_t M_BOOST_VOUT_LVL_MASK { 0x1F };
  constexpr uint16_t M_BOOST_VOUT_BASE_MV { 1800 };
  constexpr uint16_t M_BOOST_VOUT_STEP_MV { 50 };
  constexpr uint8_t M_BOOST_VOUT_LVL_MAX { 30 }; // → 3300 mV

  // BOOST.VOUTSEL
  constexpr uint8_t M_BOOST_VOUTSEL_PINREG_BIT { 0x01 };

  // BOOST.OPER.MODE
  constexpr uint8_t M_BOOST_OPER_MODE_MASK { 0x07 };
  constexpr uint8_t M_BOOST_OPER_MODE_AUTO { 0 };
  constexpr uint8_t M_BOOST_OPER_MODE_HP { 1 };
  constexpr uint8_t M_BOOST_OPER_MODE_LP { 2 };
  constexpr uint8_t M_BOOST_OPER_MODE_PT { 3 };
  constexpr uint8_t M_BOOST_OPER_MODE_NOHP { 4 };

  // BOOST.STATUS0.MODE (operational state, read-only)
  constexpr uint8_t M_BOOST_STATUS0_MODE_MASK { 0x07 };
  constexpr uint8_t M_BOOST_STATUS0_MODE_HP { 0 };
  constexpr uint8_t M_BOOST_STATUS0_MODE_LP { 1 };
  constexpr uint8_t M_BOOST_STATUS0_MODE_ULP { 2 };
  constexpr uint8_t M_BOOST_STATUS0_MODE_PT { 3 };
  constexpr uint8_t M_BOOST_STATUS0_MODE_DPS { 4 };

  // BOOST.STATUS1
  constexpr uint8_t M_BOOST_STATUS1_VOUTLVL_BIT { 1u << 3 };

  // VBATMINH (battery voltage threshold)
  constexpr uint8_t M_BOOST_VBATMINH_LVL_MASK { 0x3F };
  constexpr uint16_t M_BOOST_VBATMINH_BASE_MV { 650 };
  constexpr uint16_t M_BOOST_VBATMINH_STEP_MV { 50 };
  constexpr uint8_t M_BOOST_VBATMINH_LVL_MAX { 50 }; // → 3150 mV

  // BOOST.VBATMINHSEL
  constexpr uint8_t M_BOOST_VBATMINHSEL_BIT { 1u << 4 };

  // BOOST.CTRLSET / CTRLCLR — VBATMINSEL routing for VBAT comparator
  constexpr uint8_t M_BOOST_CTRL_VBATMINSEL_BIT { 1u << 4 };

  // ──────────────────── LDOSW (datasheet § 6.2.4) ────────────────────
  constexpr uint8_t M_LDOSW_VOUT { 0x68 };
  constexpr uint8_t M_LDOSW_LDOSW { 0x69 };
  constexpr uint8_t M_LDOSW_SEL { 0x6A };
  constexpr uint8_t M_LDOSW_CONF { 0x6C };
  constexpr uint8_t M_LDOSW_STATUS { 0x6E };

  // LDOSW.VOUT field: VOUT_LDO = 0.4 V + LVL × 0.05, LVL ∈ [8..52]
  constexpr uint8_t M_LDOSW_VOUT_LVL_MASK { 0x3F };
  constexpr uint16_t M_LDOSW_VOUT_BASE_MV { 400 };
  constexpr uint16_t M_LDOSW_VOUT_STEP_MV { 50 };
  constexpr uint8_t M_LDOSW_VOUT_LVL_MIN { 8 };  // → 800 mV
  constexpr uint8_t M_LDOSW_VOUT_LVL_MAX { 52 }; // → 3000 mV

  // LDOSW.LDOSW.ENABLE
  constexpr uint8_t M_LDOSW_ENABLE_BIT { 0x01 };

  // LDOSW.SEL fields
  constexpr uint8_t M_LDOSW_SEL_MODE_MASK { 0x01 };
  constexpr uint8_t M_LDOSW_SEL_MODE_LDO { 0 };
  constexpr uint8_t M_LDOSW_SEL_MODE_LOADSW { 1 };
  constexpr uint8_t M_LDOSW_SEL_OPER_MASK { 0x06 };
  constexpr uint8_t M_LDOSW_SEL_OPER_POS { 1 };
  constexpr uint8_t M_LDOSW_SEL_OPER_AUTO { 0 };
  constexpr uint8_t M_LDOSW_SEL_OPER_ULP { 1 };
  constexpr uint8_t M_LDOSW_SEL_OPER_HP { 2 };

  // LDOSW.STATUS bits
  constexpr uint8_t M_LDOSW_STATUS_LDO_BIT { 1u << 0 };
  constexpr uint8_t M_LDOSW_STATUS_SW_BIT { 1u << 1 };
  constexpr uint8_t M_LDOSW_STATUS_OCP_BIT { 1u << 4 };

  // ──────────────────── ADC (datasheet § 7.1.2) ────────────────────
  constexpr uint8_t M_ADC_TASKS_ADC { 0x90 };
  constexpr uint8_t M_ADC_CONFIG { 0x91 };
  constexpr uint8_t M_ADC_READVBAT { 0x96 };
  constexpr uint8_t M_ADC_READTEMP { 0x97 };
  constexpr uint8_t M_ADC_READVOUT { 0x99 };
  constexpr uint8_t M_ADC_STATUS { 0x9D };

  // ADC.CONFIG.MODE
  constexpr uint8_t M_ADC_CONFIG_MODE_INSVBAT { 0 };
  constexpr uint8_t M_ADC_CONFIG_MODE_DIETEMP { 2 };
  constexpr uint8_t M_ADC_CONFIG_MODE_VOUT { 4 };
  constexpr uint8_t M_ADC_CONFIG_MODE_MASK { 0x07 };

  // ADC.TASKS_ADC.CONV
  constexpr uint8_t M_ADC_TASKS_ADC_CONV_BIT { 0x01 };

  // MAIN events (used here for ADC ready bits)
  constexpr uint8_t M_MAIN_EVENTS_ADC_SET { 0x01 };
  constexpr uint8_t M_MAIN_EVENTS_ADC_CLR { 0x06 };

  // EVENTS_ADC bits
  constexpr uint8_t M_EVENTS_ADC_VBATRDY_BIT { 1u << 0 };
  constexpr uint8_t M_EVENTS_ADC_DIETRDY_BIT { 1u << 1 };
  constexpr uint8_t M_EVENTS_ADC_VOUTRDY_BIT { 1u << 3 };

  // ADC poll budget
  constexpr uint8_t M_ADC_MAX_POLLS { 20 };

  // Conversion constants
  constexpr float M_ADC_VBAT_FULL_SCALE_V { 3.2f };
  constexpr float M_ADC_VOUT_BASE_V { 1.8f };
  constexpr float M_ADC_VOUT_FULL_SCALE_V { 1.5f };
  constexpr float M_ADC_TEMP_OFFSET_C { 389.5f };
  constexpr float M_ADC_TEMP_SLOPE_C { 2.12f };
  constexpr float M_ADC_DENOMINATOR { 255.0f };

  // ──────────────────── TIMER (datasheet § 7.2.6) ────────────────────
  constexpr uint8_t M_TIMER_TASKS_START { 0xB0 };
  constexpr uint8_t M_TIMER_TASKS_STOP { 0xB1 };
  constexpr uint8_t M_TIMER_TASKS_KICK { 0xB2 };
  constexpr uint8_t M_TIMER_CONFIG { 0xB3 };
  constexpr uint8_t M_TIMER_TARGETHI { 0xB4 };
  constexpr uint8_t M_TIMER_TARGETMID { 0xB5 };
  constexpr uint8_t M_TIMER_TARGETLO { 0xB6 };
  constexpr uint8_t M_TIMER_STATUS { 0xB7 };

  // TIMER.CONFIG.MODE
  constexpr uint8_t M_TIMER_CONFIG_MODE_MASK { 0x03 };
  constexpr uint8_t M_TIMER_CONFIG_MODE_GENPURP { 0 };
  constexpr uint8_t M_TIMER_CONFIG_MODE_WDRST { 1 };
  constexpr uint8_t M_TIMER_CONFIG_MODE_WDPWRC { 2 };
  constexpr uint8_t M_TIMER_CONFIG_MODE_WKUP { 3 };

  constexpr uint32_t M_TIMER_MAX_VALUE { 0x00FFFFFFu };

  // MAIN events (system group)
  constexpr uint8_t M_MAIN_EVENTS_SYSTEM_SET { 0x00 };
  constexpr uint8_t M_MAIN_EVENTS_SYSTEM_CLR { 0x05 };
  constexpr uint8_t M_MAIN_INTEN_SYSTEM_SET { 0x0A };
  constexpr uint8_t M_MAIN_INTEN_SYSTEM_CLR { 0x0F };

  // MAIN events groups (continued)
  constexpr uint8_t M_MAIN_EVENTS_GPIO_SET { 0x02 };
  constexpr uint8_t M_MAIN_EVENTS_GPIO_CLR { 0x07 };
  constexpr uint8_t M_MAIN_EVENTS_BOOST_SET { 0x03 };
  constexpr uint8_t M_MAIN_EVENTS_BOOST_CLR { 0x08 };
  constexpr uint8_t M_MAIN_EVENTS_LDOSW_SET { 0x04 };
  constexpr uint8_t M_MAIN_EVENTS_LDOSW_CLR { 0x09 };

  constexpr uint8_t M_MAIN_INTEN_ADC_SET { 0x0B };
  constexpr uint8_t M_MAIN_INTEN_ADC_CLR { 0x10 };
  constexpr uint8_t M_MAIN_INTEN_GPIO_SET { 0x0C };
  constexpr uint8_t M_MAIN_INTEN_GPIO_CLR { 0x11 };
  constexpr uint8_t M_MAIN_INTEN_BOOST_SET { 0x0D };
  constexpr uint8_t M_MAIN_INTEN_BOOST_CLR { 0x12 };
  constexpr uint8_t M_MAIN_INTEN_LDOSW_SET { 0x0E };
  constexpr uint8_t M_MAIN_INTEN_LDOSW_CLR { 0x13 };

  // SystemEvent bit positions
  constexpr uint8_t M_SYSTEM_TIMER_BIT { 1u << 5 };
  constexpr uint8_t M_SYSTEM_TIMERPREWARN_BIT { 1u << 6 };
  constexpr uint8_t M_SYSTEM_TIMERFREE_BIT { 1u << 7 };

  // ──────────────────── SHIP (datasheet § 7.3.2) ────────────────────
  constexpr uint8_t M_SHIP_TASKS_SHIP { 0xC0 };
  constexpr uint8_t M_SHIP_WAKEUP { 0xC1 };
  constexpr uint8_t M_SHIP_SHPHLD { 0xC2 };

  constexpr uint8_t M_SHIP_WAKEUP_EDGE_BIT { 0x01 };      // 0=Falling, 1=Rising
  constexpr uint8_t M_SHIP_WAKEUP_HIBERNATE_BIT { 0x02 }; // 0=Pin enabled, 1=NoPin

  constexpr uint8_t M_SHIP_SHPHLD_RESISTOR_MASK { 0x03 };
  constexpr uint8_t M_SHIP_SHPHLD_CURR_MASK { 0x0C };
  constexpr uint8_t M_SHIP_SHPHLD_CURR_POS { 2 };
  constexpr uint8_t M_SHIP_SHPHLD_PULL_BIT { 1u << 4 };

  // ──────────────────── HIBERNATE (datasheet § 7.4.2) ────────────────────
  constexpr uint8_t M_HIB_TASKS_HIBER { 0xC8 };
  constexpr uint8_t M_HIB_TASKS_HIBERPT { 0xC9 };
  constexpr uint8_t M_HIB_DEBOUNCE { 0xCA };

  constexpr uint8_t M_HIB_DEBOUNCE_ENABLE_BIT { 1u << 0 };
  constexpr uint8_t M_HIB_DEBOUNCE_TIME_MASK { 0x0E };
  constexpr uint8_t M_HIB_DEBOUNCE_TIME_POS { 1 };

  // ──────────────────── GPIO (datasheet § 6.3.3) ────────────────────
  constexpr uint8_t M_GPIO_CONFIG0 { 0x80 };
  constexpr uint8_t M_GPIO_CONFIG1 { 0x81 };
  constexpr uint8_t M_GPIO_USAGE0 { 0x83 };
  constexpr uint8_t M_GPIO_USAGE1 { 0x84 };
  constexpr uint8_t M_GPIO_OUTPUT0 { 0x86 };
  constexpr uint8_t M_GPIO_OUTPUT1 { 0x87 };
  constexpr uint8_t M_GPIO_READ { 0x89 };

  constexpr uint8_t M_GPIO_CFG_INPUT_BIT { 1u << 0 };
  constexpr uint8_t M_GPIO_CFG_OUTPUT_BIT { 1u << 1 };
  constexpr uint8_t M_GPIO_CFG_OPENDRAIN_BIT { 1u << 2 };
  constexpr uint8_t M_GPIO_CFG_PULLDOWN_BIT { 1u << 3 };
  constexpr uint8_t M_GPIO_CFG_PULLUP_BIT { 1u << 4 };
  constexpr uint8_t M_GPIO_CFG_DRIVE_BIT { 1u << 5 };
  constexpr uint8_t M_GPIO_CFG_DEBOUNCE_BIT { 1u << 6 };

  constexpr uint8_t M_GPIO_USAGE_GPIO { 0 };
  constexpr uint8_t M_GPIO_USAGE_INTLO { 1 };
  constexpr uint8_t M_GPIO_USAGE_INTHI { 2 };
  constexpr uint8_t M_GPIO_USAGE_MASK { 0x03 };

  // ──────────────────── MAIN.STATUS / REQUESTSET / REQUESTCLR ────────────────────
  constexpr uint8_t M_MAIN_REQUESTSET { 0x14 };
  constexpr uint8_t M_MAIN_REQUESTCLR { 0x15 };
  constexpr uint8_t M_MAIN_STATUS { 0x16 };

  constexpr uint8_t M_MAIN_STATUS_SHPHLD_BIT { 1u << 0 };
  constexpr uint8_t M_MAIN_STATUS_PGRESET_BIT { 1u << 1 };
  constexpr uint8_t M_MAIN_STATUS_DIETEMP_BIT { 1u << 2 };

  constexpr uint8_t M_MAIN_REQUEST_DIETEMP_BIT { 1u << 0 };
  constexpr uint8_t M_MAIN_REQUEST_DIETEMPENA_BIT { 1u << 1 };
}
