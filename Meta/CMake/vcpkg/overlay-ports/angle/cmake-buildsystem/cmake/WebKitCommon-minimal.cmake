# VCPKG NOTE: A minimal version of WebKit's https://github.com/WebKit/WebKit/blob/647e67b23883960fef8890465c0f70d7ab6e63f1/Source/cmake/WebKitCommon.cmake
# To support the adapted ANGLE CMake buildsystem

# -----------------------------------------------------------------------------
# This file is included individually from various subdirectories (JSC, WTF,
# WebCore, WebKit) in order to allow scripts to build only part of WebKit.
# We want to run this file only once.
# -----------------------------------------------------------------------------
if (NOT HAS_RUN_WEBKIT_COMMON)
    set(HAS_RUN_WEBKIT_COMMON TRUE)

    if (NOT CMAKE_BUILD_TYPE)
        message(WARNING "No CMAKE_BUILD_TYPE value specified, defaulting to RelWithDebInfo.")
        set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Choose the type of build." FORCE)
    else ()
        message(STATUS "The CMake build type is: ${CMAKE_BUILD_TYPE}")
    endif ()

    # -----------------------------------------------------------------------------
    # Determine which port will be built
    # -----------------------------------------------------------------------------
    set(ALL_PORTS
        AppleWin
        Efl
        FTW
        GTK
        JSCOnly
        Mac
        PlayStation
        WPE
        WinCairo
        Linux # VCPKG EDIT: Add "Linux" so it's properly supported for ANGLE build
        Win # VCPKG EDIT: Add "Win" so it's properly supported for ANGLE build
    )
    set(PORT "NOPORT" CACHE STRING "choose which WebKit port to build (one of ${ALL_PORTS})")

    list(FIND ALL_PORTS ${PORT} RET)
    if (${RET} EQUAL -1)
        if (APPLE)
            set(PORT "Mac")
        else ()
            message(WARNING "Please choose which WebKit port to build (one of ${ALL_PORTS})")
        endif ()
    endif ()

    string(TOLOWER ${PORT} WEBKIT_PORT_DIR)

    # -----------------------------------------------------------------------------
    # Determine the compiler
    # -----------------------------------------------------------------------------
    if (${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang" OR ${CMAKE_CXX_COMPILER_ID} STREQUAL "AppleClang")
        set(COMPILER_IS_CLANG ON)
    endif ()

    if (${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
        if (${CMAKE_CXX_COMPILER_VERSION} VERSION_LESS "9.3.0")
            message(FATAL_ERROR "GCC 9.3 or newer is required to build WebKit. Use a newer GCC version or Clang.")
        endif ()
    endif ()

    if (CMAKE_COMPILER_IS_GNUCXX OR COMPILER_IS_CLANG)
        set(COMPILER_IS_GCC_OR_CLANG ON)
    endif ()

    if (MSVC AND COMPILER_IS_CLANG)
        set(COMPILER_IS_CLANG_CL ON)
    endif ()

    # -----------------------------------------------------------------------------
    # Determine the target processor
    # -----------------------------------------------------------------------------
    # Use MSVC_CXX_ARCHITECTURE_ID instead of CMAKE_SYSTEM_PROCESSOR when defined,
    # since the later one just resolves to the host processor on Windows.
    if (MSVC_CXX_ARCHITECTURE_ID)
        string(TOLOWER ${MSVC_CXX_ARCHITECTURE_ID} LOWERCASE_CMAKE_SYSTEM_PROCESSOR)
    else ()
        string(TOLOWER ${CMAKE_SYSTEM_PROCESSOR} LOWERCASE_CMAKE_SYSTEM_PROCESSOR)
    endif ()
    if (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "(^aarch64|^arm64|^cortex-?[am][2-7][2-8])")
        set(WTF_CPU_ARM64 1)
    elseif (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "(^arm|^cortex)")
        set(WTF_CPU_ARM 1)
    elseif (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "^mips64")
        set(WTF_CPU_MIPS64 1)
    elseif (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "^mips")
        set(WTF_CPU_MIPS 1)
    elseif (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "(x64|x86_64|amd64)")
        # FORCE_32BIT is set in the build script when --32-bit is passed
        # on a Linux/intel 64bit host. This allows us to produce 32bit
        # binaries without setting the build up as a crosscompilation,
        # which is the only way to modify CMAKE_SYSTEM_PROCESSOR.
        if (FORCE_32BIT)
            set(WTF_CPU_X86 1)
        else ()
            set(WTF_CPU_X86_64 1)
        endif ()
    elseif (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "(i[3-6]86|x86)")
        set(WTF_CPU_X86 1)
    elseif (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "ppc")
        set(WTF_CPU_PPC 1)
    elseif (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64")
        set(WTF_CPU_PPC64 1)
    elseif (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64le")
        set(WTF_CPU_PPC64LE 1)
    elseif (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "^riscv64")
        set(WTF_CPU_RISCV64 1)
    elseif (LOWERCASE_CMAKE_SYSTEM_PROCESSOR MATCHES "^loongarch64")
        set(WTF_CPU_LOONGARCH64 1)
    else ()
        set(WTF_CPU_UNKNOWN 1)
    endif ()

    # -----------------------------------------------------------------------------
    # Determine the operating system
    # -----------------------------------------------------------------------------
    if (UNIX)
        if (APPLE)
            set(WTF_OS_MAC_OS_X 1)
        elseif (CMAKE_SYSTEM_NAME MATCHES "Linux")
            set(WTF_OS_LINUX 1)
        else ()
            set(WTF_OS_UNIX 1)
        endif ()
    elseif (CMAKE_SYSTEM_NAME MATCHES "Windows")
        set(WTF_OS_WINDOWS 1)
    elseif (CMAKE_SYSTEM_NAME MATCHES "Fuchsia")
        set(WTF_OS_FUCHSIA 1)
    else ()
        message(FATAL_ERROR "Unknown OS '${CMAKE_SYSTEM_NAME}'")
    endif ()

    # -----------------------------------------------------------------------------
    # Default library types
    # -----------------------------------------------------------------------------

    set(CMAKE_POSITION_INDEPENDENT_CODE True)

    # -----------------------------------------------------------------------------
    # Default output directories, which can be overwritten by ports
    #------------------------------------------------------------------------------
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    # -----------------------------------------------------------------------------
    # Find common packages (used by all ports)
    # -----------------------------------------------------------------------------
    if (WIN32)
        list(APPEND CMAKE_PROGRAM_PATH $ENV{SystemDrive}/cygwin/bin)
    endif ()

    # -----------------------------------------------------------------------------
    # Helper macros and feature defines
    # -----------------------------------------------------------------------------

    # To prevent multiple inclusion, most modules should be included once here.
    include(CheckCCompilerFlag)
    include(CheckCXXCompilerFlag)
    include(CheckCXXSourceCompiles)
    include(CheckFunctionExists)
    include(CheckIncludeFile)
    include(CheckSymbolExists)
    include(CheckStructHasMember)
    include(CheckTypeSize)
    include(CMakeDependentOption)
    include(CMakeParseArguments)
    include(CMakePushCheckState)
    include(ProcessorCount)

    # include(WebKitPackaging)
    include(WebKitMacros-minimal)
    # include(WebKitFS)
    # include(WebKitCCache)
    include(WebKitCompilerFlags-minimal)
    # include(WebKitStaticAnalysis)
    # include(WebKitFeatures)
    # include(WebKitFindPackage)

    # include(OptionsCommon)
    # include(Options${PORT})

    # -----------------------------------------------------------------------------
    # Job pool to avoid running too many memory hungry linker processes
    # -----------------------------------------------------------------------------
    if (${CMAKE_BUILD_TYPE} STREQUAL "Release" OR ${CMAKE_BUILD_TYPE} STREQUAL "MinSizeRel")
        set_property(GLOBAL PROPERTY JOB_POOLS link_pool_jobs=4)
    else ()
        set_property(GLOBAL PROPERTY JOB_POOLS link_pool_jobs=2)
    endif ()
    set(CMAKE_JOB_POOL_LINK link_pool_jobs)

endif ()
