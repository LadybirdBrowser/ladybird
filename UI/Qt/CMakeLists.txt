set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

if (APPLE)
    # The native presentation paths use platform GPU buffer handles directly
    # instead of copying frames through CPU memory.
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
endif()

qt_add_executable(ladybird main.cpp)
target_sources(ladybird PRIVATE
    Application.cpp
    Autocomplete.cpp
    BookmarksBar.cpp
    BrowserWindow.cpp
    ChromeStyle.cpp
    DevToolsBanner.cpp
    EventLoopImplementationQt.cpp
    EventLoopImplementationQtEventTarget.cpp
    FindInPageWidget.cpp
    Icon.cpp
    LocationEdit.cpp
    Menu.cpp
    Settings.cpp
    StringUtils.cpp
    Tab.cpp
    TabBar.cpp
    WebContentView.cpp
    WindowControlButton.cpp
    ladybird.qrc
)
target_link_libraries(ladybird PRIVATE Qt::Core Qt::Gui Qt::Widgets)

if (APPLE)
    target_sources(ladybird PRIVATE
        MacWindow.mm
        WebContentViewMac.mm
    )
    target_link_libraries(ladybird PRIVATE Qt::GuiPrivate "-framework Cocoa" "-framework IOSurface" "-framework Metal" "-framework QuartzCore")
endif()

if (NOT APPLE AND USE_VULKAN_DMABUF_IMAGES AND Qt6_VERSION VERSION_GREATER_EQUAL 6.7.0)
    set(WEB_CONTENT_VIEW_LINUX_VERT_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/WebContentViewLinux.vert")
    set(WEB_CONTENT_VIEW_LINUX_FRAG_SHADER "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/WebContentViewLinux.frag")
    set(WEB_CONTENT_VIEW_LINUX_VERT_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/WebContentViewLinuxVertShader.h")
    set(WEB_CONTENT_VIEW_LINUX_FRAG_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/WebContentViewLinuxFragShader.h")

    add_custom_command(
        OUTPUT "${WEB_CONTENT_VIEW_LINUX_VERT_SHADER_HEADER}"
        COMMAND ${CMAKE_COMMAND}
            -DLOG=${CMAKE_CURRENT_BINARY_DIR}/glslang-webcontent-vert.log
            -P "${LADYBIRD_SOURCE_DIR}/Meta/CMake/run_quiet.cmake"
            --
            "${GLSLANG_VALIDATOR}"
            -V
            --vn webcontent_vert_shader_spv
            -o "${WEB_CONTENT_VIEW_LINUX_VERT_SHADER_HEADER}"
            "${WEB_CONTENT_VIEW_LINUX_VERT_SHADER}"
        DEPENDS "${WEB_CONTENT_VIEW_LINUX_VERT_SHADER}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${WEB_CONTENT_VIEW_LINUX_FRAG_SHADER_HEADER}"
        COMMAND ${CMAKE_COMMAND}
            -DLOG=${CMAKE_CURRENT_BINARY_DIR}/glslang-webcontent-frag.log
            -P "${LADYBIRD_SOURCE_DIR}/Meta/CMake/run_quiet.cmake"
            --
            "${GLSLANG_VALIDATOR}"
            -V
            --vn webcontent_frag_shader_spv
            -o "${WEB_CONTENT_VIEW_LINUX_FRAG_SHADER_HEADER}"
            "${WEB_CONTENT_VIEW_LINUX_FRAG_SHADER}"
        DEPENDS "${WEB_CONTENT_VIEW_LINUX_FRAG_SHADER}"
        VERBATIM
    )
    add_custom_target(generate_web_content_view_linux_shaders
        DEPENDS
            "${WEB_CONTENT_VIEW_LINUX_VERT_SHADER_HEADER}"
            "${WEB_CONTENT_VIEW_LINUX_FRAG_SHADER_HEADER}"
    )

    target_sources(ladybird PRIVATE
        WebContentViewLinux.cpp
        "${WEB_CONTENT_VIEW_LINUX_VERT_SHADER_HEADER}"
        "${WEB_CONTENT_VIEW_LINUX_FRAG_SHADER_HEADER}"
    )
    target_include_directories(ladybird PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(ladybird PRIVATE Vulkan::Vulkan)
    add_dependencies(ladybird generate_web_content_view_linux_shaders)
endif()

create_ladybird_bundle(ladybird)

if (WIN32)
    ladybird_windows_bin(ladybird CONSOLE)
    qt_generate_deploy_script(
        TARGET ladybird
        OUTPUT_SCRIPT ladybird_deploy_script
        CONTENT "
qt_deploy_runtime_dependencies(
    EXECUTABLE $<TARGET_FILE:ladybird>
    PLUGINS_DIR $<TARGET_FILE_DIR:ladybird>
    NO_TRANSLATIONS
)
")

    add_custom_command(TARGET ladybird POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -P ${ladybird_deploy_script}
        COMMAND_EXPAND_LISTS
    )
endif()
