cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
            "../../components/managed_vendor"
            "../../components/tts_engine"
            "../../components/tts_engine_provider_xfyun"
            "../../components/claw_modules"
            "../../components/lua_modules"
            "../../components/claw_capabilities"
            )
if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(basic_demo)

fatfs_create_spiflash_image(storage fatfs_image FLASH_IN_PROJECT)
