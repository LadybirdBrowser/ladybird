if (NOT TARGET webgpu_dawn)
    add_library(webgpu_dawn INTERFACE IMPORTED)
    set_target_properties(webgpu_dawn PROPERTIES
        INTERFACE_COMPILE_OPTIONS "--use-port=${CMAKE_CURRENT_LIST_DIR}/emdawnwebgpu.port.py"
        INTERFACE_LINK_OPTIONS "--use-port=${CMAKE_CURRENT_LIST_DIR}/emdawnwebgpu.port.py"
    )
    add_library(dawn::webgpu_dawn ALIAS webgpu_dawn)
endif()
