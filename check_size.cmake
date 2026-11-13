# Fails the build if the firmware image would reach into the FatFs partition.
file(SIZE "${BIN}" size)
if(size GREATER LIMIT)
    message(FATAL_ERROR "Firmware is ${size} bytes, the limit is ${LIMIT} (src/flash_fs.h).")
endif()
message(STATUS "Firmware: ${size} of ${LIMIT} bytes")
