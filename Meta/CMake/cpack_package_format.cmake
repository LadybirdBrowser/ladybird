# Runs while CPack packages, once the files of the package are staged. A Linux package says which kind it is, so that
# the browser in it can tell how to update it: the same build can also end up in an archive.
set(resource_directory "${CPACK_TEMPORARY_DIRECTORY}/Browser${CPACK_PACKAGING_INSTALL_PREFIX}/${CPACK_LADYBIRD_RESOURCE_DIRECTORY}")
set(marker "${resource_directory}/package-format")
file(WRITE "${marker}" "${CPACK_GENERATOR}\n")
file(CHMOD "${marker}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)

# A package can end up on another release of the distribution than the one that built it, so it also says which
# release that was, in the format of os-release. Like any reader of os-release, it looks in /etc before /usr/lib, and
# without either file the package goes without.
foreach (os_release_path IN ITEMS /etc/os-release /usr/lib/os-release)
    if (EXISTS "${os_release_path}")
        file(STRINGS "${os_release_path}" os_release REGEX "^(ID|VERSION_ID)=")
        list(JOIN os_release "\n" os_release)
        set(target "${resource_directory}/package-target")
        file(WRITE "${target}" "${os_release}\n")
        file(CHMOD "${target}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
        break()
    endif()
endforeach()
