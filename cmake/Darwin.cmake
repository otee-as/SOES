# Darwin (macOS) platform support – only simulation variant is supported.
# Physical LAN9252 hardware is not available on macOS.

if(SIM_VARIANT)
  set (SOES_DEMO applications/linux_sim)
  set(HAL_SOURCES
	${SOES_SOURCE_DIR}/soes/hal/linux-sim/esc_hw.c
	${SOES_SOURCE_DIR}/soes/hal/linux-sim/esc_hw.h
	${SOES_SOURCE_DIR}/soes/hal/linux-sim/esc_hw_eep.c
	)
  set(HAL_INCLUDES
	${SOES_SOURCE_DIR}/soes/hal/linux-sim
	)
else()
  message(FATAL_ERROR "On macOS (Darwin), only SIM_VARIANT is supported. "
    "Use: cmake -DSIM_VARIANT=ON ..")
endif()

include_directories(
  ${SOES_SOURCE_DIR}/soes/include/sys/gcc
  ${SOES_SOURCE_DIR}/${SOES_DEMO}
  )

# Common compile flags – relaxed compared to Linux since these are upstream
# warnings in the SOES code that only surface with AppleClang.
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-sign-conversion -Wno-unused-function)
