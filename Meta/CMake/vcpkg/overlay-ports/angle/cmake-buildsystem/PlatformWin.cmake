# We're targeting Windows 10 which will have DirectX 11 on it so require that
# but make DirectX 9 optional

list(APPEND ANGLE_DEFINITIONS
    GL_APICALL=
    GL_API=
    NOMINMAX
)

# We're targeting Windows 10 which will have DirectX 11
list(APPEND ANGLE_SOURCES
    ${d3d11_backend_sources}
    ${d3d_shared_sources}

    ${angle_translator_hlsl_sources}

    ${libangle_gpu_info_util_sources}
    ${libangle_gpu_info_util_win_sources}
)

list(APPEND ANGLE_DEFINITIONS
    ANGLE_ENABLE_D3D11
    ANGLE_ENABLE_HLSL
    # VCPKG EDIT: add ANGLE_PRELOADED_D3DCOMPILER_MODULE_NAMES
    "-DANGLE_PRELOADED_D3DCOMPILER_MODULE_NAMES={ \"d3dcompiler_47.dll\", \"d3dcompiler_46.dll\", \"d3dcompiler_43.dll\" }"
)

# https://issues.angleproject.org/issues/345274916
list(APPEND ANGLEGLESv2_LIBRARIES dxguid dxgi synchronization)

if(NOT angle_is_winuwp) # vcpkg EDIT: Exclude DirectX 9 on UWP
    # DirectX 9 support should be optional but ANGLE will not compile without it
    list(APPEND ANGLE_SOURCES ${d3d9_backend_sources})
    list(APPEND ANGLE_DEFINITIONS ANGLE_ENABLE_D3D9)
    list(APPEND ANGLEGLESv2_LIBRARIES d3d9)
endif()

# VCPKG EDITS:

# Do not specify library type here

# Handle angle_enable_d3d11_compositor_native_window defines

if(angle_enable_d3d11_compositor_native_window)
	list(APPEND ANGLE_DEFINITIONS ANGLE_ENABLE_D3D11_COMPOSITOR_NATIVE_WINDOW)
endif()

# OpenGL backend

if(USE_OPENGL)
    # Enable GLSL compiler output.
    list(APPEND ANGLE_DEFINITIONS ANGLE_ENABLE_GLSL)

    if(USE_ANGLE_EGL OR ENABLE_WEBGL)
        list(APPEND ANGLE_SOURCES
            ${gl_backend_sources}

            ${libangle_gl_egl_dl_sources}
            ${libangle_gl_egl_sources}
            ${libangle_gl_sources}
        )

        list(APPEND ANGLE_DEFINITIONS
            ANGLE_ENABLE_OPENGL
            ANGLE_ENABLE_GL_DESKTOP_BACKEND
        )
    endif()
endif()
