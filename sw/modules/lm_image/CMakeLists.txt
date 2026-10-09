# Language model image for boards with an lm partition (see ../../README.md).
#
# This is a Zephyr module rather than part of the application's CMakeLists.txt
# because the flash runner configuration is written while Zephyr itself is
# configured, before the application's own CMake code runs.

dt_nodelabel(lm_node NODELABEL lm_partition)
if(NOT lm_node)
  return()
endif()

set(app ${APPLICATION_SOURCE_DIR})
dt_reg_addr(lm_addr PATH ${lm_node})
dt_reg_size(lm_size PATH ${lm_node})
set(lm_hex ${ZEPHYR_BINARY_DIR}/lm.hex)
set(merged_hex ${ZEPHYR_BINARY_DIR}/merged.hex)

add_custom_command(
  OUTPUT ${lm_hex}
  COMMAND ${PYTHON_EXECUTABLE} ${app}/tools/lmbuild/pack.py
    --bin ${ZEPHYR_BINARY_DIR}/lm.bin --hex ${lm_hex}
    --address ${lm_addr} --max-size ${lm_size}
    de=${app}/lm/de.lm en=${app}/lm/en.lm
  DEPENDS ${app}/lm/de.lm ${app}/lm/en.lm ${app}/tools/lmbuild/pack.py
)
add_custom_command(
  OUTPUT ${merged_hex}
  COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/mergehex.py
    -o ${merged_hex} ${ZEPHYR_BINARY_DIR}/${KERNEL_HEX_NAME} ${lm_hex}
  DEPENDS ${lm_hex} ${ZEPHYR_BINARY_DIR}/${KERNEL_HEX_NAME}
)
add_custom_target(lm_image ALL DEPENDS ${merged_hex})
add_dependencies(lm_image zephyr_final)
set_property(TARGET runners_yaml_props_target PROPERTY hex_file ${merged_hex})
