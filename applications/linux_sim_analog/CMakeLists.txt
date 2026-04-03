
add_executable (sim_analog
  main.c
  slave_objectlist.c
  ${SOES_SOURCE_DIR}/soes/hal/linux-sim/esc_ecat.c
  )
target_include_directories(sim_analog PRIVATE
  ${SOES_SOURCE_DIR}/soes/hal/linux-sim
  )
target_link_libraries(sim_analog LINK_PUBLIC soes pthread)
