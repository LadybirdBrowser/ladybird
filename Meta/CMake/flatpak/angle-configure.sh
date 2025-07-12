#!/usr/bin/env bash

set -e

export PATH=$PWD/depot_tools:$PATH
cd angle

gn gen out --args='
    is_official_build=true
    is_component_build=true
    is_debug=false
    angle_build_tests=false
    angle_enable_renderdoc=false
    angle_enable_swiftshader=false
    angle_enable_vulkan=true
    angle_enable_wgpu=false
    angle_expose_non_conformant_extensions_and_versions=true
    angle_use_wayland=true
    angle_use_x11=false
    build_angle_deqp_tests=false
    use_custom_libcxx=false
    use_safe_libstdcxx=true
    chrome_pgo_phase=0
    is_cfi = false
'
