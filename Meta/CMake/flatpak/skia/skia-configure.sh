#!/usr/bin/env bash

set -e

gn gen out --args='
    is_official_build=true
    is_component_build=true
    is_debug=false
    skia_use_dng_sdk=false
    skia_use_wuffs=false
    skia_use_zlib=true
    skia_use_system_zlib=true
    skia_use_harfbuzz=true
    skia_use_vulkan=true
    skia_use_fontconfig=true
    skia_use_icu=true
    skia_use_system_icu=true
    extra_cflags=["-DSK_USE_EXTERNAL_VULKAN_HEADERS","-Wno-psabi"]
    extra_cflags_cc=["-DSKCMS_DLL"]
'
