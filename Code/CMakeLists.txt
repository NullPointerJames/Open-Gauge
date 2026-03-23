cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(open_gauge)

# Create SPIFFS image from data directory and flash it with project
spiffs_create_partition_image(spiffs data FLASH_IN_PROJECT)
