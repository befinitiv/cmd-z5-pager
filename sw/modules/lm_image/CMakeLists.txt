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
# With MCUboot the signed application is flashed (written by the signing
# step after zephyr_final).
if(CONFIG_BOOTLOADER_MCUBOOT)
  set(app_hex ${ZEPHYR_BINARY_DIR}/${KERNEL_NAME}.signed.hex)
else()
  set(app_hex ${ZEPHYR_BINARY_DIR}/${KERNEL_HEX_NAME})
endif()

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
    -o ${merged_hex} ${app_hex} ${lm_hex}
  DEPENDS ${lm_hex} ${app_hex}
)
add_custom_target(lm_image ALL DEPENDS ${merged_hex})
add_dependencies(lm_image zephyr_final)

# `west flash` writes merged.hex. With MCUboot, the signing script sets the
# runner's hex file after the modules, and cmake/flash turns it into the
# runners.yaml text right away; that text is only written at generate time,
# so the hex file line is replaced there at the end of Zephyr's configuration.
set_property(GLOBAL PROPERTY cmdz5_merged_hex ${merged_hex})
function(cmdz5_lm_runner_hex)
  get_property(hex GLOBAL PROPERTY cmdz5_merged_hex)
  get_target_property(yaml runners_yaml_props_target yaml_contents)
  string(REGEX REPLACE "hex_file: [^\n]*" "hex_file: ${hex}" yaml "${yaml}")
  set_target_properties(runners_yaml_props_target PROPERTIES yaml_contents "${yaml}")
endfunction()
cmake_language(DEFER DIRECTORY ${ZEPHYR_BASE} CALL cmdz5_lm_runner_hex)
