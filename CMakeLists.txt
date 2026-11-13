# ===========================================================================
# PicoBoyGB for RP2040 (2 MB flash): Waveshare 2" ST7789 240x320 device
# and the old PicoBoy Color
# ---------------------------------------------------------------------------
# Game Boy DMG emulator (Peanut-GB). Port of the PicoBoy Color Plus SDK build
# (pbc-plus/picoboygb, RP2350/16 MB) back to the RP2040 with 2 MB flash.
# Display and buttons: see src/picoboy_pins.h.
#
#   ./build.sh            or by hand:
#   cmake -S . -B build -DPICO_SDK_PATH=... && cmake --build build -j
# ===========================================================================
cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_PLATFORM)
    set(PICO_PLATFORM rp2040)
endif()
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico)
endif()

# ---- tuning knobs ----------------------------------------------------------
# 200 MHz at 1.15 V: officially supported by SDK 2.1+ on the RP2040.
# Flash = 200 / FLASH_CLKDIV = 100 MHz (W25Q: 133 MHz max).
# Display SPI: clk_peri (200 MHz) / even divider -> requesting 62.5 MHz gives
# 50 MHz; a full 200x180 frame then takes 11.5 ms (sent from core 1).
set(CPU_KHZ      "200000"            CACHE STRING "System clock in kHz")
set(CPU_VREG     "VREG_VOLTAGE_1_15" CACHE STRING "Core voltage (VREG_VOLTAGE_1_10 .. VREG_VOLTAGE_1_30)")
set(FLASH_CLKDIV "2"                 CACHE STRING "Flash SPI clock divider, Waveshare device (even: 2 or 4)")
set(FLASH_CLKDIV_PBC "4"             CACHE STRING "Flash SPI clock divider, PicoBoy Color (4 = 50 MHz at 200 MHz)")
set(LCD_SPI_HZ   "62500000"          CACHE STRING "ST7789 SPI clock request in Hz")

include(pico_sdk_import.cmake)

project(picoboygb_rp2040 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# 2 MB flash; firmware 256 KB, FatFs 1.69 MB, saves 64 KB (src/flash_fs.h).
set(PICO_FLASH_SIZE_BYTES 2097152)

pico_sdk_init()

# boot2 for the W25Q-series flash of the Pico board profile; it must see the
# flash clock divider below.
target_compile_definitions(bs2_default PRIVATE PICO_FLASH_SPI_CLKDIV=${FLASH_CLKDIV})

# One firmware per device, same sources (pins: src/picoboy_pins.h):
#   picoboygb      RP2040 + Waveshare 2" ST7789 240x320, single buttons
#   picoboygb_pbc  old PicoBoy Color (RP2040, 2 MB), ST7789 240x280, joystick
function(add_picoboygb TARGET BOARD_PICOBOY FLASH_DIV)
    add_executable(${TARGET}
        src/main.cpp
        src/minigb_apu.cpp
        src/flash_fs.c
        src/usb_descriptors.c
        src/usb_msc.c
        fatfs/ff.c
        fatfs/ffunicode.c
        fatfs/diskio.c
    )

    target_include_directories(${TARGET} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src        # tusb_config.h, headers
        ${CMAKE_CURRENT_LIST_DIR}/fatfs      # ff.h, ffconf.h, diskio.h
    )

    target_compile_definitions(${TARGET} PRIVATE
        PB_BOARD_PICOBOY=${BOARD_PICOBOY}
        PICO_FLASH_SIZE_BYTES=2097152
        # SDK 2.2 moves clk_peri to the fixed 48 MHz USB clock whenever the
        # system clock is changed; keep it on the system clock so the display
        # SPI can be sysclk / 4 = 50 MHz.
        PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
        # Read by boot2 (second stage bootloader): flash clock = sysclk / div.
        PICO_FLASH_SPI_CLKDIV=${FLASH_DIV}
        # The overclock may need more time for the crystal / PLL to settle.
        PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
        SYS_CLOCK_KHZ=${CPU_KHZ}
        VREG_BOOST_LEVEL=${CPU_VREG}
        ST7789_SPI_HZ=${LCD_SPI_HZ}u
        # Half the PBC+ rate: the 44.1 kHz timer interrupt costs the
        # Cortex-M0+ noticeably more, and the piezo does not reproduce the
        # difference. (Only used where there is a speaker.)
        # Must be 1e6 / whole microseconds: the output timer ticks every
        # 45 us = 22 222 Hz. With 22 050 the buffer ran dry after a few
        # seconds and every frame lost samples (constant crackle).
        AUDIO_SAMPLE_RATE=22222
    )

    target_compile_options(${TARGET} PRIVATE -O2)

    target_link_libraries(${TARGET}
        pico_stdlib
        pico_multicore
        pico_unique_id
        hardware_spi
        hardware_dma
        hardware_pwm
        hardware_flash
        hardware_sync
        hardware_clocks
        hardware_vreg
        hardware_watchdog
        tinyusb_device
        tinyusb_board
    )

    # USB is used for mass storage, not for stdio.
    pico_enable_stdio_usb(${TARGET} 0)
    pico_enable_stdio_uart(${TARGET} 0)

    pico_add_extra_outputs(${TARGET})

    # The firmware must stay below the FatFs partition (256 KB, flash_fs.h),
    # otherwise the first format of the drive would overwrite code.
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE_DIR:${TARGET}>/${TARGET}.bin
                -DLIMIT=262144 -P ${CMAKE_CURRENT_LIST_DIR}/check_size.cmake
        VERBATIM)
endfunction()

add_picoboygb(picoboygb     0 ${FLASH_CLKDIV})
add_picoboygb(picoboygb_pbc 1 ${FLASH_CLKDIV_PBC})

# The PicoBoy Color gets its own boot2 with a slower flash clock: at
# 200 MHz / 2 = 100 MHz its flash misread code and ROM (game restarted at
# once, back to the menu), while the Pico board of the Waveshare device is fine.
pico_define_boot_stage2(bs2_pbc ${PICO_SDK_PATH}/src/rp2040/boot_stage2/boot2_w25q080.S)
target_compile_definitions(bs2_pbc PRIVATE PICO_FLASH_SPI_CLKDIV=${FLASH_CLKDIV_PBC})
pico_set_boot_stage2(picoboygb_pbc bs2_pbc)
