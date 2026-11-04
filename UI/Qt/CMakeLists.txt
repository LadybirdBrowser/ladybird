set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if (LINUX AND NOT ANDROID)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
    # NB: Without the Wayland client library, a tab dragged out of a window does not take a window along with it.
    pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
    # NB: Without the Wayland scanner, the browser cannot ask the Wayland compositor which GPU it presents on.
    pkg_check_modules(WAYLAND_SCANNER wayland-scanner)
    pkg_check_modules(XCB_ICCCM REQUIRED IMPORTED_TARGET xcb-icccm)
endif()

add_library(ladybird_impl STATIC
    AllTabsPopover.cpp
    Application.cpp
    Autocomplete.cpp
    BookmarkDialog.cpp
    BookmarkPopover.cpp
    BookmarksBar.cpp
    BrowserWindow.cpp
    ChromeStyle.cpp
    CrashReportNotifier.cpp
    CrashReportReviewWidget.cpp
    DevToolsBanner.cpp
    EventLoopImplementationQt.cpp
    EventLoopImplementationQtEventTarget.cpp
    FindInPageWidget.cpp
    HiddenPageSizing.cpp
    Icon.cpp
    InputMethodUtils.cpp
    JavaScriptDialog.cpp
    LadybirdButton.cpp
    LocationEdit.cpp
    Menu.cpp
    NativeTextEditingLineEdit.cpp
    NativeWindowContainer.cpp
    PerformanceMonitorWidget.cpp
    PermissionPromptPopover.cpp
    PictureInPictureWindow.cpp
    PlainText.cpp
    Popover.cpp
    ProcessManagerWindow.cpp
    ReportReview.cpp
    RestoreSessionCard.cpp
    ScreenshotSelectionOverlay.cpp
    ScreenshotViewer.cpp
    SelectDropdown.cpp
    SiteSettingsPopover.cpp
    StringUtils.cpp
    SystemAppearance.cpp
    Tab.cpp
    TabBar.cpp
    Theme.cpp
    WebCompatReportReview.cpp
    WebContentView.cpp
    WindowCloseCoordinator.cpp
    WindowControlButton.cpp
    WindowScreenObserver.cpp
    WindowToolbar.cpp
)
if (NOT APPLE AND NOT WIN32 AND NOT ANDROID)
    target_sources(ladybird_impl PRIVATE
        AccessibilityInterface.cpp
    )
endif()

target_link_libraries(ladybird_impl PUBLIC Qt::Core Qt::Gui Qt::Widgets)

if (TARGET Qt6::Positioning)
    target_sources(ladybird_impl PRIVATE GeolocationProviderQt.cpp GeolocationProviderQt.h)
    target_link_libraries(ladybird_impl PUBLIC Qt::Positioning)
    target_compile_definitions(ladybird_impl PUBLIC LADYBIRD_QT_HAVE_POSITIONING=1)
endif()

if (LINUX AND NOT ANDROID)
    target_sources(ladybird_impl PRIVATE ExternalURLActivationToken.cpp ExternalURLHandler.cpp X11Window.cpp X11Window.h)
    target_link_libraries(ladybird_impl PRIVATE LibThreading PkgConfig::GIO PkgConfig::XCB_ICCCM Qt::GuiPrivate)
    if (WAYLAND_CLIENT_FOUND)
        target_sources(ladybird_impl PRIVATE WaylandWindow.cpp WaylandWindow.h)
        target_link_libraries(ladybird_impl PRIVATE PkgConfig::WAYLAND_CLIENT)
        target_compile_definitions(ladybird_impl PUBLIC LADYBIRD_HAS_WAYLAND_CLIENT)
    endif()
    if (WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER_FOUND AND USE_VULKAN_DMABUF_IMAGES)
        pkg_get_variable(WAYLAND_SCANNER_EXECUTABLE wayland-scanner wayland_scanner)
        foreach(protocol linux-dmabuf-v1 viewporter)
            set(protocol_xml "${CMAKE_CURRENT_SOURCE_DIR}/Protocols/${protocol}.xml")
            set(protocol_header "${CMAKE_CURRENT_BINARY_DIR}/${protocol}-client-protocol.h")
            set(protocol_code "${CMAKE_CURRENT_BINARY_DIR}/${protocol}-protocol.c")
            add_custom_command(
                OUTPUT "${protocol_header}"
                COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${protocol_xml}" "${protocol_header}"
                DEPENDS "${protocol_xml}"
                VERBATIM
            )
            add_custom_command(
                OUTPUT "${protocol_code}"
                COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${protocol_xml}" "${protocol_code}"
                DEPENDS "${protocol_xml}"
                VERBATIM
            )
            target_sources(ladybird_impl PRIVATE "${protocol_header}" "${protocol_code}")
        endforeach()
        target_sources(ladybird_impl PRIVATE
            WaylandGpu.cpp
            WaylandGpu.h
            WaylandPageSurface.cpp
            WaylandPageSurface.h
            WaylandRegistry.cpp
            WaylandRegistry.h
        )
        target_include_directories(ladybird_impl PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_compile_definitions(ladybird_impl PUBLIC LADYBIRD_HAS_WAYLAND_GPU_QUERY)
    else()
        message(WARNING "Building without the Wayland page surface. Pages on Wayland will use a fallback that can fall "
            "behind the window as it resizes. The page surface needs the wayland-client and wayland-scanner pkg-config "
            "modules (libwayland-dev on Debian and Ubuntu), Vulkan and glslangValidator.")
    endif()
endif()

if (APPLE)
    target_sources(ladybird_impl PRIVATE
        ApplicationIcon.mm
        Conversions.mm
        DictionaryLookup.mm
        ExternalURLHandler.mm
        LadybirdAccessibilityElement.mm
        MacWindow.mm
        MacWindowFocus.mm
        NativeTextEditingBindings.mm
        Menu.mm
        ScrollbarPreferencesMac.mm
        SpellCheckerMac.mm
        SystemAppearanceMac.mm
        TranslateIconMac.mm
        WebContentViewAccessibility.mm
        WebContentViewMac.mm
    )
    target_link_libraries(ladybird_impl PRIVATE Qt::GuiPrivate "-framework ApplicationServices" "-framework Cocoa" "-framework IOSurface" "-framework QuartzCore")
    set_source_files_properties(
        LadybirdAccessibilityElement.mm
        SpellCheckerMac.mm
        TranslateIconMac.mm
        TranslatorMac.mm
        WebContentViewAccessibility.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )

    # The Translation framework has no Objective-C API, so the browser reaches it through a small Swift library. The
    # API it uses came in the macOS 26.4 SDK, and CMake builds Swift for one architecture at a time. So a build with an
    # older SDK, or a universal build, leaves the translator out, and the browser doesn't offer to translate pages.
    include(CheckLanguage)
    check_language(Swift)
    list(LENGTH CMAKE_OSX_ARCHITECTURES ladybird_architecture_count)
    set(ladybird_sdk "${CMAKE_OSX_SYSROOT}")
    if (NOT ladybird_sdk)
        set(ladybird_sdk macosx)
    endif()
    execute_process(COMMAND xcrun --sdk "${ladybird_sdk}" --show-sdk-version OUTPUT_VARIABLE ladybird_sdk_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (CMAKE_Swift_COMPILER AND ladybird_architecture_count LESS_EQUAL 1 AND ladybird_sdk_version VERSION_GREATER_EQUAL 26.4)
        set(ladybird_swift_architecture "${CMAKE_SYSTEM_PROCESSOR}")
        if (CMAKE_OSX_ARCHITECTURES)
            set(ladybird_swift_architecture "${CMAKE_OSX_ARCHITECTURES}")
        endif()
        set(CMAKE_Swift_COMPILER_TARGET "${ladybird_swift_architecture}-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}")
        enable_language(Swift)
        # NB: Whatever links the library keeps linking as C++, so it keeps the link options that only apply to C and C++
        #     links.
        set(CMAKE_Swift_LINKER_PREFERENCE_PROPAGATES OFF)
        add_library(ladybird_translation_service STATIC TranslationServiceMac.swift)
        target_sources(ladybird_impl PRIVATE TranslatorMac.mm)
        target_link_libraries(ladybird_impl PRIVATE ladybird_translation_service "-framework NaturalLanguage")
        target_compile_definitions(ladybird_impl PUBLIC LADYBIRD_HAS_TRANSLATOR)
        # The framework only exists from macOS 15, and the browser runs on older versions without it.
        target_link_options(ladybird_impl INTERFACE "LINKER:-weak_framework,Translation")
    else()
        message(STATUS "Building without page translation, which needs Swift, the macOS 26.4 SDK, and a single architecture")
    endif()
endif()

if (NOT APPLE AND USE_VULKAN_DMABUF_IMAGES)
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

    target_sources(ladybird_impl PRIVATE
        WebContentViewLinux.cpp
        "${WEB_CONTENT_VIEW_LINUX_VERT_SHADER_HEADER}"
        "${WEB_CONTENT_VIEW_LINUX_FRAG_SHADER_HEADER}"
    )
    target_include_directories(ladybird_impl PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(ladybird_impl PRIVATE Vulkan::Vulkan)
    add_dependencies(ladybird_impl generate_web_content_view_linux_shaders)
endif()

if (HAS_DIRECTX)
    target_sources(ladybird_impl PRIVATE WebContentViewWindows.cpp)
    target_link_libraries(ladybird_impl PRIVATE Qt::GuiPrivate d3d11)
endif()

qt_add_executable(ladybird main.cpp ladybird.qrc)
target_link_libraries(ladybird PRIVATE ladybird_impl)

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
