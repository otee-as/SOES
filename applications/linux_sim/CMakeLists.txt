
add_executable (sim_demo
  main.c
  slave_objectlist.c
  ${SOES_SOURCE_DIR}/soes/hal/linux-sim/esc_ecat.c
  )
target_include_directories(sim_demo PRIVATE
  ${SOES_SOURCE_DIR}/soes/hal/linux-sim
  )
target_link_libraries(sim_demo LINK_PUBLIC soes pthread)
