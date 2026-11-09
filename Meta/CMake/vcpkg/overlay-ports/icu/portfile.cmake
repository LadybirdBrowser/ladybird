# NB: This is vcpkg's own icu port, which packages the ICU data as a file of its own and links a stub library in its
#     place. To update ICU, update the version in vcpkg.json here.
set(CURRENT_PORT_DIR "${VCPKG_ROOT_DIR}/ports/icu")

if (NOT VCPKG_TARGET_IS_ANDROID AND NOT VCPKG_TARGET_IS_EMSCRIPTEN)
    set(ladybird_icu_data_stash "${CURRENT_BUILDTREES_DIR}/ladybird-icu-data")

    # FIXME: Send the patch upstream. A shared build that ships the data as an archive writes its list of data files
    #        into a directory nothing creates.
    set(ladybird_icu_patch "${CMAKE_CURRENT_LIST_DIR}/create-data-list-directory.patch")
    macro(vcpkg_extract_source_archive)
        _vcpkg_extract_source_archive(${ARGN} "${ladybird_icu_patch}")
    endmacro()

    macro(vcpkg_make_configure)
        set(ladybird_icu_configure_arguments ${ARGN})
        list(FIND ladybird_icu_configure_arguments OPTIONS ladybird_icu_options_index)
        if (ladybird_icu_options_index EQUAL -1)
            message(FATAL_ERROR "The icu port no longer passes OPTIONS to vcpkg_make_configure()")
        endif()
        math(EXPR ladybird_icu_options_index "${ladybird_icu_options_index} + 1")
        list(INSERT ladybird_icu_configure_arguments ${ladybird_icu_options_index} --with-data-packaging=archive)
        _vcpkg_make_configure(${ladybird_icu_configure_arguments})
    endmacro()

    # The port deletes share/ after installing, and the data file is installed there.
    macro(vcpkg_make_install)
        _vcpkg_make_install(${ARGN})
        file(GLOB_RECURSE ladybird_icu_data_files "${CURRENT_PACKAGES_DIR}/share/icudt*.dat")
        if (NOT ladybird_icu_data_files)
            message(FATAL_ERROR "ICU installed no data file")
        endif()
        file(REMOVE_RECURSE "${ladybird_icu_data_stash}")
        file(COPY ${ladybird_icu_data_files} DESTINATION "${ladybird_icu_data_stash}")
    endmacro()
endif()

include("${CURRENT_PORT_DIR}/portfile.cmake")

if (DEFINED ladybird_icu_data_stash)
    file(GLOB ladybird_icu_data_files "${ladybird_icu_data_stash}/icudt*.dat")
    file(INSTALL ${ladybird_icu_data_files} DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
endif()
