vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ami-iit/serial_cpp
    REF "v${VERSION}"
    SHA512 3f3bf55b2dc437c36f82aadf5df329877d6beddf1d47b09984610ea83d931635d61daea711c46ae2df9b70eede45e1cc4fe6e2fd55b45f8510b813d361797e6a
    HEAD_REF master
)

vcpkg_configure_cmake(SOURCE_PATH ${SOURCE_PATH})

vcpkg_install_cmake()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/serial_cpp")
vcpkg_fixup_pkgconfig()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(MAKE_DIRECTORY ${CURRENT_PACKAGES_DIR}/share/serial_cpp)
file(WRITE ${CURRENT_PACKAGES_DIR}/share/serial_cpp/serial_cpp-config.cmake
        "include(\${CMAKE_CURRENT_LIST_DIR}/../serial-cpp/serial_cppConfig.cmake)")
file(WRITE ${CURRENT_PACKAGES_DIR}/share/serial_cpp/serial_cppConfig.cmake
        "include(\${CMAKE_CURRENT_LIST_DIR}/../serial-cpp/serial_cppConfig.cmake)")
