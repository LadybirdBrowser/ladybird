set(VCPKG_BUILD_TYPE release)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_C_FLAGS "")

if (WIN32)
    set(VCPKG_CXX_FLAGS "/GR")
else()
    set(VCPKG_CXX_FLAGS "-frtti")
endif()
