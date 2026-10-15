vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO jeremy-rifkin/cpptrace
    REF "v${VERSION}"
    SHA512 e88edddbcdd423d49ed3adb02cf70580ee3a56065db4d81ca69d3f9f6d9b64ac27734842ca3b6d8ff45a548c25900a88f979e39d777af422a153e586d26ac5b5
    HEAD_REF main
    PATCHES
        emscripten-platform.diff
        delay.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        libunwind CPPTRACE_UNWIND_WITH_LIBUNWIND
)

if(VCPKG_TARGET_IS_EMSCRIPTEN)
    # Emscripten exposes no stack-unwinding or symbol back-end, so build a no-op tracer.
    set(BACKEND_OPTIONS
        -DCPPTRACE_UNWIND_WITH_NOTHING=ON
        -DCPPTRACE_GET_SYMBOLS_WITH_NOTHING=ON
    )
else()
    set(BACKEND_OPTIONS
        -DCPPTRACE_USE_EXTERNAL_LIBDWARF=ON
    )
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCPPTRACE_USE_EXTERNAL_ZSTD=ON
        -DCPPTRACE_VCPKG=ON
        -DCPPTRACE_DELAYLOAD_DBGHELP=ON
        ${BACKEND_OPTIONS}
        ${FEATURE_OPTIONS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME "cpptrace"
    CONFIG_PATH "lib/cmake/cpptrace"
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
