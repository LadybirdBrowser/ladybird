set(ladybird_helper_processes
    Compositor
    ImageDecoder
    RequestServer
    WebContent
    WebWorker
)

if (ENABLE_CRANELIFT_JIT)
    list(APPEND ladybird_helper_processes WasmCompiler)
endif()

# Kills the helpers when the Browser is gone, which the kernel cannot do on macOS.
if (APPLE)
    list(APPEND ladybird_helper_processes ProcessReaper)
endif()

# On macOS, helpers run with the hardened runtime, so the kernel refuses to run code from memory that was writable, except
# in the MAP_JIT regions where the renderers put the WebAssembly code that Cranelift compiles. Library validation stays
# off, since development builds sign their libraries ad hoc, without a team identifier. Every build of the helper signs
# it, so that targeted builds keep these protections too.
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

    set(plist "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n")
    foreach (entitlement IN LISTS entitlements)
        string(APPEND plist "\t<key>${entitlement}</key>\n\t<true/>\n")
    endforeach()
    string(APPEND plist "</dict>\n</plist>\n")
    set(entitlements_file "${CMAKE_CURRENT_BINARY_DIR}/${target}.entitlements")
    file(WRITE "${entitlements_file}" "${plist}")

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND codesign -s - -f -o runtime --entitlements "${entitlements_file}" "$<TARGET_FILE:${target}>"
        VERBATIM
    )
endfunction()
