#ifndef BOARD_H
#define BOARD_H

#include "floppy.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "mfm.h"
#include "pico/stdlib.h"

#define BOARD_SYS_CLOCK_KHZ (2u * MFM_READ_PIO_HZ / 1000u)

#define FLOPPY_PINS_DEFAULT \
  {.index = 14, .track0 = 5, .write_protect = 4, .read_data = 3, \
   .disk_change = 1, .drive_select = 12, .motor_enable = 10, .direction = 9, \
   .step = 8, .write_data = 7, .write_gate = 6, .side_select = 2, .density = 15}

#define FLOPPY_PINS_ALT \
  {.index = 16, .track0 = 22, .write_protect = 13, .read_data = 26, \
   .disk_change = 28, .drive_select = 14, .motor_enable = 17, .direction = 18, \
   .step = 19, .write_data = 20, .write_gate = 21, .side_select = 27, .density = 15}

#if defined(PICO_RP2040) && PICO_RP2040
#define BOARD_NAME "RP2040 (Pico)"
#elif defined(PICO_RP2350) && PICO_RP2350
#define BOARD_NAME "RP2350 (Pico 2)"
#else
#error unsupported Raspberry Pi silicon
#endif

static inline bool board_clock_init(void) {
#if defined(PICO_RP2040) && PICO_RP2040
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  sleep_ms(2);
#endif
  return set_sys_clock_khz(BOARD_SYS_CLOCK_KHZ, true) &&
         clock_get_hz(clk_sys) == BOARD_SYS_CLOCK_KHZ * 1000u;
}

#endif
