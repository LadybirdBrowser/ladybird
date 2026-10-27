set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Widgets)

if (NOT APPLE)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Positioning)
endif()

if (APPLE OR (LINUX AND NOT ANDROID) OR HAS_DIRECTX)
    # The native presentation paths use platform GPU buffer handles directly instead of copying frames through CPU memory.
    # Wayland activation also uses Qt platform APIs.
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    find_package(Qt6 6.10 REQUIRED COMPONENTS GuiPrivate)
endif()

if (LINUX AND NOT ANDROID)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
    # NB: Without the Wayland client library, a tab dragged out of a window does not take a window along with it.
    pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
    # NB: Without the Wayland scanner, the browser cannot ask the Wayland compositor which GPU it presents on.
    pkg_check_modules(WAYLAND_SCANNER wayland-scanner)
    pkg_check_modules(XCB_ICCCM REQUIRED IMPORTED_TARGET xcb-icccm)
endif()

qt_add_executable(ladybird main.cpp)
target_sources(ladybird PRIVATE
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
    Icon.cpp
    HiddenPageSizing.cpp
    InputMethodUtils.cpp
    JavaScriptDialog.cpp
    LadybirdButton.cpp
    LocationEdit.cpp
    Menu.cpp
    NativeWindowContainer.cpp
    NativeTextEditingLineEdit.cpp
    ProcessManagerWindow.cpp
    PerformanceMonitorWidget.cpp
    PermissionPromptPopover.cpp
    PictureInPictureWindow.cpp
    Popover.cpp
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
    ladybird.qrc
)
if (NOT APPLE AND NOT WIN32 AND NOT ANDROID)
    target_sources(ladybird PRIVATE
        AccessibilityInterface.cpp
    )
endif()

target_link_libraries(ladybird PRIVATE Qt::Core Qt::Gui Qt::Widgets)

if (TARGET Qt6::Positioning)
    target_sources(ladybird PRIVATE GeolocationProviderQt.cpp GeolocationProviderQt.h)
    target_link_libraries(ladybird PRIVATE Qt::Positioning)
    target_compile_definitions(ladybird PRIVATE LADYBIRD_QT_HAVE_POSITIONING=1)
endif()

if (LINUX AND NOT ANDROID)
    target_sources(ladybird PRIVATE ExternalURLActivationToken.cpp ExternalURLHandler.cpp X11Window.cpp X11Window.h)
    target_link_libraries(ladybird PRIVATE LibThreading PkgConfig::GIO PkgConfig::XCB_ICCCM Qt::GuiPrivate)
    if (WAYLAND_CLIENT_FOUND)
        target_sources(ladybird PRIVATE WaylandWindow.cpp WaylandWindow.h)
        target_link_libraries(ladybird PRIVATE PkgConfig::WAYLAND_CLIENT)
        target_compile_definitions(ladybird PRIVATE LADYBIRD_HAS_WAYLAND_CLIENT)
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
            target_sources(ladybird PRIVATE "${protocol_header}" "${protocol_code}")
        endforeach()
        target_sources(ladybird PRIVATE
            WaylandGpu.cpp
            WaylandGpu.h
            WaylandPageSurface.cpp
            WaylandPageSurface.h
            WaylandRegistry.cpp
            WaylandRegistry.h
        )
        target_include_directories(ladybird PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_compile_definitions(ladybird PRIVATE LADYBIRD_HAS_WAYLAND_GPU_QUERY)
    endif()
endif()

if (APPLE)
    target_sources(ladybird PRIVATE
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
        WebContentViewAccessibility.mm
        WebContentViewMac.mm
    )
    target_link_libraries(ladybird PRIVATE Qt::GuiPrivate "-framework ApplicationServices" "-framework Cocoa" "-framework IOSurface" "-framework QuartzCore")
    set_source_files_properties(
        LadybirdAccessibilityElement.mm
        SpellCheckerMac.mm
        WebContentViewAccessibility.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
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

    target_sources(ladybird PRIVATE
        WebContentViewLinux.cpp
        "${WEB_CONTENT_VIEW_LINUX_VERT_SHADER_HEADER}"
        "${WEB_CONTENT_VIEW_LINUX_FRAG_SHADER_HEADER}"
    )
    target_include_directories(ladybird PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(ladybird PRIVATE Vulkan::Vulkan)
    add_dependencies(ladybird generate_web_content_view_linux_shaders)
endif()

if (HAS_DIRECTX)
    target_sources(ladybird PRIVATE WebContentViewWindows.cpp)
    target_link_libraries(ladybird PRIVATE Qt::GuiPrivate d3d11)
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

if (BUILD_TESTING AND NOT BUILD_BROWSER_TESTS_ONLY)
    if (APPLE)
        add_library(NativeTextEditingTestMain OBJECT
            ${LADYBIRD_SOURCE_DIR}/Tests/UI/Qt/NativeTextEditingTestMain.mm
            ${LADYBIRD_SOURCE_DIR}/Libraries/LibTest/AssertionHandler.cpp
        )
        target_link_libraries(NativeTextEditingTestMain PRIVATE AK LibTest "-framework AppKit")
        ladybird_test("${LADYBIRD_SOURCE_DIR}/Tests/UI/Qt/TestNativeTextEditingBindings.mm" UI/Qt
            NO_TEST
            CUSTOM_MAIN "$<TARGET_OBJECTS:NativeTextEditingTestMain>"
            LIBS LibWebCommon Qt::Widgets "-framework AppKit"
        )
        foreach(mode default custom)
            add_test(NAME TestNativeTextEditingBindings-${mode}
                COMMAND ${Python3_EXECUTABLE}
                    ${LADYBIRD_SOURCE_DIR}/Tests/UI/Qt/run-native-text-editing-tests.py
                    $<TARGET_FILE:TestNativeTextEditingBindings> ${mode}
            )
        endforeach()
    endif()

    add_executable(TestPerformanceMonitorWidget
        ${LADYBIRD_SOURCE_DIR}/Tests/LibWebView/TestPerformanceMonitorWidget.cpp
        ChromeStyle.cpp
        PerformanceMonitorWidget.cpp
        StringUtils.cpp
        SystemAppearance.cpp
        Theme.cpp
    )
    if (APPLE)
        target_sources(TestPerformanceMonitorWidget PRIVATE SystemAppearanceMac.mm)
        target_link_libraries(TestPerformanceMonitorWidget PRIVATE "-framework AppKit")
    endif()
    set_target_properties(TestPerformanceMonitorWidget PROPERTIES AUTOMOC ON)
    target_include_directories(TestPerformanceMonitorWidget PRIVATE ${LADYBIRD_SOURCE_DIR})
    target_link_libraries(TestPerformanceMonitorWidget PRIVATE Qt6::Widgets LibWebView LibURL AK)
    qt_import_plugins(TestPerformanceMonitorWidget INCLUDE Qt6::QOffscreenIntegrationPlugin)
    add_test(NAME TestPerformanceMonitorWidget COMMAND TestPerformanceMonitorWidget)
    set_tests_properties(TestPerformanceMonitorWidget PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
