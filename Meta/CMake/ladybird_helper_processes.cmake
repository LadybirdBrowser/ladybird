set(ladybird_helper_processes
    AudioServer
    Compositor
    ImageDecoder
    MediaServer
    RequestServer
    VideoDecoderServer
    WebContent
    WebWorker
)

# Kills the helpers when the Browser is gone, which the kernel cannot do on macOS.
if (APPLE)
    list(APPEND ladybird_helper_processes ProcessReaper)
endif()

function(ladybird_write_entitlements path)
    set(plist "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n")
    foreach (entitlement IN LISTS ARGN)
        string(APPEND plist "\t<key>${entitlement}</key>\n\t<true/>\n")
    endforeach()
    string(APPEND plist "</dict>\n</plist>\n")
    file(WRITE "${path}" "${plist}")
endfunction()

# On macOS, helpers run with the hardened runtime, so the kernel refuses to run code from memory that was writable, except
# in the MAP_JIT regions where the renderers put the code that their JIT compilers generate. Library validation stays
# off, since development builds sign their libraries ad hoc, without a team identifier. Every build of the helper signs
# it, so that targeted builds keep these protections too. Release signing uses the .release.entitlements file instead:
# every library in a release bundle carries the same team identifier, so library validation stays on, and debuggers
# must not be able to attach.
function(ladybird_sign_helper_process target)
    if (NOT APPLE)
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 ARG "ALLOW_JIT" "" "")

    set(entitlements com.apple.security.cs.disable-library-validation)
    if (ARG_ALLOW_JIT)
        list(APPEND entitlements com.apple.security.cs.allow-jit)
    endif()
    if (CMAKE_BUILD_TYPE MATCHES "Debug|RelWithDebInfo")
        list(APPEND entitlements com.apple.security.get-task-allow)
    endif()

    set(entitlements_file "${CMAKE_CURRENT_BINARY_DIR}/${target}.entitlements")
    ladybird_write_entitlements("${entitlements_file}" ${entitlements})

    set(release_entitlements)
    if (ARG_ALLOW_JIT)
        list(APPEND release_entitlements com.apple.security.cs.allow-jit)
    endif()
    ladybird_write_entitlements("${CMAKE_CURRENT_BINARY_DIR}/${target}.release.entitlements" ${release_entitlements})
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${target}.release.entitlements" DESTINATION entitlements
        COMPONENT ladybird_Runtime)

    ladybird_codesign_command(codesign_command "${target}" "${entitlements_file}" -o runtime "$<TARGET_FILE:${target}>")
    add_custom_command(TARGET ${target} POST_BUILD COMMAND ${codesign_command} VERBATIM)
endfunction()
