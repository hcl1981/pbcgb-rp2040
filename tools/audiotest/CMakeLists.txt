# Tontest fuer den alten PicoBoy Color (RP2040): C-Dur-Tonleiter auf GP15,
# einmal als Rechteckton (wie tone()), einmal wie die Tonausgabe des
# Game-Boy-Emulators (schneller PWM-Traeger, Sinus um einen Ruhepegel).
# Display-Treiber und Pins kommen aus ../../src.
#   cmake -S tools/audiotest -B tools/audiotest/build && cmake --build tools/audiotest/build
cmake_minimum_required(VERSION 3.13)

set(PICO_PLATFORM rp2040)
set(PICO_BOARD pico)

include(pico_sdk_import.cmake)

project(pbc_audiotest C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pbc_audiotest main.cpp)

target_include_directories(pbc_audiotest PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../src)
target_compile_definitions(pbc_audiotest PRIVATE
    PB_BOARD_PICOBOY=1
    ST7789_SPI_HZ=62500000u
)
target_link_libraries(pbc_audiotest pico_stdlib hardware_spi hardware_pwm)

pico_add_extra_outputs(pbc_audiotest)
