cmake_minimum_required(VERSION 3.13.1)
include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)
project(Spark_AIrplay_1)

FILE(GLOB app_sources ../src/*.c ../src/*.cpp ../src/crypto/*.c)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE ../include ../include/mbedtls)
target_compile_definitions(app PRIVATE -DMBEDTLS_CONFIG_FILE=\"mbedtls_config.h\")
