# decidio runtime firmware (ESP-IDF >= 5.3). Prebuilt binaries ship in decidio/firmware_bins/; rebuild only after C changes:
#   decidio build-firmware --board xiao_sense|s3cam     (inside the ESP-IDF shell)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(decidio_rt)
