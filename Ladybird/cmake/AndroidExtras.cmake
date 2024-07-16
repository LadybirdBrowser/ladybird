# Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
#
# SPDX-License-Identifier: BSD-2-Clause
#

#
# Copy resources into tarball for inclusion in /assets of APK
#
set(LADYBIRD_RESOURCE_ROOT "${LADYBIRD_SOURCE_DIR}/Base/res")
macro(copy_res_folder folder)
    add_custom_target(copy-${folder}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${LADYBIRD_RESOURCE_ROOT}/${folder}"
            "asset-bundle/res/${folder}"
    )
    add_dependencies(archive-assets copy-${folder})
endmacro()
add_custom_target(archive-assets COMMAND ${CMAKE_COMMAND} -E chdir asset-bundle tar cf ../ladybird-assets.tar ./ )
copy_res_folder(ladybird)
copy_res_folder(html)
copy_res_folder(fonts)
copy_res_folder(icons)
copy_res_folder(emoji)
copy_res_folder(themes)
add_custom_target(copy-autoplay-allowlist
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LADYBIRD_SOURCE_DIR}/Base/home/anon/.config/BrowserAutoplayAllowlist.txt"
        "asset-bundle/res/ladybird/BrowserAutoplayAllowlist.txt"
)
add_custom_target(copy-content-filters
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LADYBIRD_SOURCE_DIR}/Base/home/anon/.config/BrowserContentFilters.txt"
        "asset-bundle/res/ladybird/BrowserContentFilters.txt"
)
add_custom_target(copy-certs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${Lagom_BINARY_DIR}/cacert.pem"
        "asset-bundle/res/ladybird/cacert.pem"
)
add_dependencies(archive-assets copy-autoplay-allowlist copy-content-filters copy-certs)
add_custom_target(copy-assets COMMAND ${CMAKE_COMMAND} -E copy_if_different ladybird-assets.tar "${CMAKE_SOURCE_DIR}/Ladybird/Android/src/main/assets/")
add_dependencies(copy-assets archive-assets)
add_dependencies(ladybird copy-assets)
