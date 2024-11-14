include_guard()

include(fontconfig)

find_package(unofficial-skia CONFIG)
if(unofficial-skia_FOUND)
    set(SKIA_TARGET unofficial::skia::skia)
    if (HAS_FONTCONFIG)
        set(CMAKE_LINK_GROUP_USING_no_as_needed_SUPPORTED TRUE)
        set(CMAKE_LINK_GROUP_USING_no_as_needed "LINKER:--push-state,--no-as-needed" "LINKER:--pop-state")
        set_property(TARGET unofficial::skia::skia APPEND PROPERTY INTERFACE_LINK_LIBRARIES "$<LINK_GROUP:no_as_needed,Fontconfig::Fontconfig>")
    endif()
else()
    find_package(PkgConfig)

    # Get skia version from vcpkg.json
    file(READ ${LADYBIRD_SOURCE_DIR}/vcpkg.json VCPKG_DOT_JSON)
    string(JSON VCPKG_OVERRIDES_LENGTH LENGTH ${VCPKG_DOT_JSON} overrides)
    MATH(EXPR VCPKG_OVERRIDES_END_RANGE "${VCPKG_OVERRIDES_LENGTH}-1")
    foreach(IDX RANGE ${VCPKG_OVERRIDES_END_RANGE})
      string(JSON VCPKG_OVERRIDE_NAME GET ${VCPKG_DOT_JSON} overrides ${IDX} name)
      if(VCPKG_OVERRIDE_NAME STREQUAL "skia")
        string(JSON SKIA_REQUIRED_VERSION GET ${VCPKG_DOT_JSON} overrides ${IDX} version)
        string(REGEX MATCH "[0-9]+" SKIA_REQUIRED_VERSION ${SKIA_REQUIRED_VERSION})
      endif()
    endforeach()

    pkg_check_modules(skia skia=${SKIA_REQUIRED_VERSION} REQUIRED IMPORTED_TARGET skia)
    set(SKIA_TARGET PkgConfig::skia)
endif()
swizzle_target_properties_for_swift(${SKIA_TARGET})
add_library(skia ALIAS ${SKIA_TARGET})
