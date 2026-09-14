# ESP-IDF project for the NDW BL Router.
#
# Targets esp32c3 specifically. The bootloader offset differs by chip family
# (0x0 here, 0x1000 on the original ESP32), which is why the firmware manifest
# publishes one build per chip rather than one image with a widened regex.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ndw-bl-router)
