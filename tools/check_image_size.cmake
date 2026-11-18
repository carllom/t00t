# Fails the build if the firmware image would reach the user-bank region at
# the end of flash (src/patch/user_bank.h): flashing such an image would
# overwrite every module's stored Patches.
#   cmake -DIMAGE=<t00t.bin> -DLIMIT=<region start offset> -P check_image_size.cmake
file(SIZE "${IMAGE}" size)
math(EXPR limit "${LIMIT}")
if(size GREATER limit)
    message(FATAL_ERROR "${IMAGE} is ${size} bytes and would overwrite the user-bank region at ${LIMIT}")
endif()
