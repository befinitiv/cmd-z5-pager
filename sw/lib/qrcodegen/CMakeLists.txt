# Nayuki's QR Code generator (MIT, see LICENSE.txt), taken from the copy in
# the LVGL module (modules/lib/gui/lvgl/src/libs/qrcode) without its LVGL
# includes. Used for the channel QR codes.
zephyr_include_directories(include)
target_sources(app PRIVATE src/qrcodegen.c)
