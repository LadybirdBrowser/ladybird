set(_supported_webgpu_backends "Dawn" "Null")
set(_default_webgpu_backend "Dawn")

# Note: ladybird_option() doesn't play nicely with this variable, so we use set() directly.
set(LADYBIRD_WEBGPU_BACKEND ${_default_webgpu_backend} CACHE STRING "The WebGPU backend to use for the ladybird application. Possible values: ${_supported_webgpu_backends}")

if (NOT "${LADYBIRD_WEBGPU_BACKEND}" IN_LIST _supported_webgpu_backends)
    message(FATAL_ERROR "Invalid value for LADYBIRD_WEBGPU_BACKEND: ${LADYBIRD_WEBGPU_BACKEND}. Possible values: ${_supported_webgpu_backends}")
endif()
