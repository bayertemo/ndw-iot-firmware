# ESP-IDF project for the NDW Test Button.
#
# A meter stand-in: broadcasts a counter that increments on each press, so a
# whole pipeline can be proven with a number you can verify by eye.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ndw-test-button)
