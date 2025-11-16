set(VCPKG_BUILD_TYPE release)
if (NOT DEFINED VCPKG_LIBRARY_LINKAGE)
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()

set(VCPKG_C_FLAGS "")

if (WIN32)
    set(VCPKG_CXX_FLAGS "/GR")
else()
    set(VCPKG_CXX_FLAGS "-frtti")
endif()
