include(cmake/ResourceFiles.cmake)

function(create_ladybird_bundle target_name)
    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME "Ladybird"
        MACOSX_BUNDLE_GUI_IDENTIFIER org.ladybird.Ladybird
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        MACOSX_BUNDLE_INFO_PLIST "${LADYBIRD_SOURCE_DIR}/UI/Info.plist"
        MACOSX_BUNDLE TRUE
        WIN32_EXECUTABLE TRUE
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER org.ladybird.Ladybird
    )

    if (APPLE)
        set(bundle_dir "$<TARGET_BUNDLE_DIR:${target_name}>")
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${bundle_dir}/Contents/Resources"
            COMMAND "iconutil" --convert icns "${LADYBIRD_SOURCE_DIR}/UI/Icons/macos/app_icon.iconset" --output "${bundle_dir}/Contents/Resources/app_icon.icns"
        )

        # Note: This symlink is removed in the install commands
        # This makes the bundle in the build directory *NOT* relocatable
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E create_symlink "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}" "${bundle_dir}/Contents/lib"
        )

        if (NOT CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo" AND "arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND codesign -s - -v -f --entitlements "${LADYBIRD_SOURCE_DIR}/Meta/debug.plist" "${bundle_dir}"
            )
        else()
            add_custom_target(apply-debug-entitlements
                COMMAND codesign -s - -v -f --entitlements "${LADYBIRD_SOURCE_DIR}/Meta/debug.plist" "${bundle_dir}"
                USES_TERMINAL
            )
        endif()
    endif()

    if (APPLE)
        set(resource_base_dir "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Resources")
    else()
        set(resource_base_dir "${CMAKE_BINARY_DIR}/${IN_BUILD_PREFIX}${CMAKE_INSTALL_DATADIR}/Lagom")
    endif()

    copy_resources_to_build(${resource_base_dir} ${target_name})
endfunction()

# Select UI Framework
if (ENABLE_QT)
   add_subdirectory(Qt)
elseif (APPLE)
   add_subdirectory(AppKit)
elseif(ANDROID)
   add_subdirectory(Android)
else()
    # TODO: Check for other GUI frameworks here when we move them in-tree
    return()
endif()

if (NOT TARGET ladybird)
    message(FATAL_ERROR "UI Framework selection must declare a ladybird target")
endif()

if (TARGET ladybird_impl)
    set(LADYBIRD_TARGET ladybird_impl PUBLIC)
else()
    set(LADYBIRD_TARGET ladybird PRIVATE)
endif()

set(LADYBIRD_LIBS AK LibCore LibFileSystem LibGfx LibImageDecoderClient LibIPC LibJS LibMain LibWeb LibWebView LibRequests LibURL)
target_link_libraries(${LADYBIRD_TARGET} PRIVATE ${LADYBIRD_LIBS})
target_link_libraries(${LADYBIRD_TARGET} PRIVATE OpenSSL::Crypto OpenSSL::SSL)

target_include_directories(${LADYBIRD_TARGET} ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(${LADYBIRD_TARGET} ${LADYBIRD_SOURCE_DIR})
target_include_directories(${LADYBIRD_TARGET} ${LADYBIRD_SOURCE_DIR}/Services/)

function(set_helper_process_properties)
    set(targets ${ARGV})
    if (APPLE)
        # Store helper processes in the same bundle directory as the main application
        set_target_properties(${targets} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:ladybird>")
    else()
        set_target_properties(${targets} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${IN_BUILD_PREFIX}${CMAKE_INSTALL_LIBEXECDIR}")
    endif()
endfunction()

add_custom_target(run
    COMMAND "${CMAKE_COMMAND}" -E env "LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR}" "$<TARGET_FILE:ladybird>" $ENV{LAGOM_ARGS}
    USES_TERMINAL
    VERBATIM
)

if (APPLE)
    add_custom_target(debug-ladybird
        COMMAND "${CMAKE_COMMAND}" -E env "LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR}" lldb "$<TARGET_BUNDLE_DIR:ladybird>"
        USES_TERMINAL
    )
else()
    add_custom_target(debug-ladybird
        COMMAND "${CMAKE_COMMAND}" -E env "LADYBIRD_SOURCE_DIR=${LADYBIRD_SOURCE_DIR}" gdb "$<TARGET_FILE:ladybird>"
        USES_TERMINAL
    )
endif()

add_subdirectory(Headless)

set(ladybird_helper_processes ImageDecoder RequestServer WebContent WebWorker)

add_dependencies(ladybird ${ladybird_helper_processes})
add_dependencies(headless-browser ${ladybird_helper_processes})
add_dependencies(WebDriver ladybird headless-browser)

set_helper_process_properties(${ladybird_helper_processes})
if (APPLE)
    set_helper_process_properties(headless-browser WebDriver)
endif()

if(NOT CMAKE_SKIP_INSTALL_RULES)
    include(cmake/InstallRules.cmake)
endif()
