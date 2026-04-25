# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(WeatherSensor)

# Use simple display test (comment out for LVGL version)
# target_sources(app PRIVATE src/main_simple.cpp)

# Use LVGL version
target_sources(app PRIVATE src/main.cpp)

target_include_directories(app PRIVATE inc)

# -fpermissive needed for Zephyr networking headers (C-style void* casts)
# when compiled as C++ code.
target_compile_options(app PRIVATE -fpermissive)
