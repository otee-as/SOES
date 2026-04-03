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
elseif(RPI_VARIANT)
  set (SOES_DEMO applications/raspberry_lan9252demo)
  set(HAL_SOURCES
	${SOES_SOURCE_DIR}/soes/hal/raspberrypi-lan9252/esc_hw.c
	${SOES_SOURCE_DIR}/soes/hal/raspberrypi-lan9252/esc_hw.h
	)
else()
  set(SOES_DEMO applications/linux_lan9252demo)
  set(HAL_SOURCES
	${SOES_SOURCE_DIR}/soes/hal/linux-lan9252/esc_hw.c
	)
endif()

include_directories(
  ${SOES_SOURCE_DIR}/soes/include/sys/gcc
  ${SOES_SOURCE_DIR}/${SOES_DEMO}
  )

# Common compile flags
add_compile_options(-Wall -Wextra -Wconversion -Wno-unused-parameter -Wno-unused-function -Werror)
