# Runs while CPack packages, once the files of the package are staged. A Linux package says which kind it is, so that
# the browser in it can tell how to update it: the same build can also end up in an archive.
set(marker "${CPACK_TEMPORARY_DIRECTORY}/Browser${CPACK_PACKAGING_INSTALL_PREFIX}/${CPACK_LADYBIRD_RESOURCE_DIRECTORY}/package-format")
file(WRITE "${marker}" "${CPACK_GENERATOR}\n")
file(CHMOD "${marker}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
