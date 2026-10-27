find_package(Qt6 QUIET COMPONENTS Core)
if (NOT Qt6_FOUND)
    return()
endif()

add_library(QtEventLoopTestMain OBJECT
    QtEventLoopTestMain.cpp
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibTest/AssertionHandler.cpp"
)
target_link_libraries(QtEventLoopTestMain PRIVATE AK LibCore LibTest Qt::Core)

ladybird_test("TestEventLoopImplementation.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtEventLoopTestMain>"
    LIBS LibThreading Qt::Core
)
target_sources(TestEventLoopImplementation PRIVATE
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/EventLoopImplementationQt.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/EventLoopImplementationQtEventTarget.cpp"
)
set_target_properties(TestEventLoopImplementation PROPERTIES AUTOMOC ON)

ladybird_test("TestFullscreenDebounce.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtEventLoopTestMain>"
    LIBS LibThreading Qt::Core
)
target_sources(TestFullscreenDebounce PRIVATE
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/EventLoopImplementationQt.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/EventLoopImplementationQtEventTarget.cpp"
)
set_target_properties(TestFullscreenDebounce PROPERTIES AUTOMOC ON)

find_package(Qt6 QUIET COMPONENTS Gui Widgets)
if (NOT Qt6_FOUND)
    return()
endif()

add_library(QtTestMain OBJECT
    QtTestMain.cpp
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibTest/AssertionHandler.cpp"
)
target_link_libraries(QtTestMain PRIVATE AK LibCore LibTest Qt::Core Qt::Gui Qt::Widgets)

# QtTestMain runs its tests on Qt's offscreen platform plugin, so every test that links it must call this. A static Qt
# only links the default platform plugin into an executable, so the offscreen plugin is imported. On Windows, Qt only
# searches bin/platforms, where the ladybird target deploys just the "windows" plugin, so the offscreen plugin is copied
# there too. Without it, Qt shows a modal message box that blocks the test until CTest's timeout kills it.
function(deploy_qt_offscreen_platform_plugin target)
    qt_import_plugins(${target} INCLUDE Qt6::QOffscreenIntegrationPlugin)
    if (NOT WIN32)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
            "$<TARGET_FILE_DIR:${target}>/platforms/"
        VERBATIM
    )
endfunction()

# Crash reports are only saved on macOS and Linux.
if (APPLE OR LINUX)
    ladybird_test("TestCrashReportReviewWidget.cpp" UI/Qt
        CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
        LIBS LibFileSystem LibURL LibWebView Qt::Core Qt::Gui Qt::Widgets
    )
    target_sources(TestCrashReportReviewWidget PRIVATE
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/ChromeStyle.cpp"
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/CrashReportReviewWidget.cpp"
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/ReportReview.cpp"
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
    )
    deploy_qt_offscreen_platform_plugin(TestCrashReportReviewWidget)
endif()

ladybird_test("TestScreenshotSelectionOverlay.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS LibURL Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestScreenshotSelectionOverlay PRIVATE
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ChromeStyle.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/Icon.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/Popover.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ScreenshotSelectionOverlay.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
)
deploy_qt_offscreen_platform_plugin(TestScreenshotSelectionOverlay)

ladybird_test("TestScreenshotViewer.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS LibURL Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestScreenshotViewer PRIVATE
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ChromeStyle.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ScreenshotViewer.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
)
deploy_qt_offscreen_platform_plugin(TestScreenshotViewer)

ladybird_test("TestWebCompatReportReview.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS LibURL LibWebView Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestWebCompatReportReview PRIVATE
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ChromeStyle.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/Icon.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ReportReview.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/WebCompatReportReview.cpp"
)
deploy_qt_offscreen_platform_plugin(TestWebCompatReportReview)

ladybird_test("TestHiddenPageSizing.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestHiddenPageSizing PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/HiddenPageSizing.cpp")
deploy_qt_offscreen_platform_plugin(TestHiddenPageSizing)

ladybird_test("TestSelectDropdown.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS LibWeb LibGfx LibIPC LibURL Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestSelectDropdown PRIVATE
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/SelectDropdown.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
)
deploy_qt_offscreen_platform_plugin(TestSelectDropdown)

ladybird_test("TestProcessManagerWindow.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS LibWebView LibURL Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestProcessManagerWindow PRIVATE
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibWebView/ProcessMonitor.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/Icon.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ChromeStyle.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ProcessManagerWindow.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
)
target_compile_definitions(TestProcessManagerWindow PRIVATE TEST_RESOURCE_ROOT="${LADYBIRD_SOURCE_DIR}/Base/res")
deploy_qt_offscreen_platform_plugin(TestProcessManagerWindow)


ladybird_test("TestNativeTextEditingLineEdit.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestNativeTextEditingLineEdit PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/NativeTextEditingLineEdit.cpp")
if (APPLE)
    target_sources(TestNativeTextEditingLineEdit PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/NativeTextEditingBindings.mm" "${LADYBIRD_SOURCE_DIR}/UI/Qt/MacWindowFocus.mm")
    target_link_libraries(TestNativeTextEditingLineEdit PRIVATE "-framework AppKit")
endif()
deploy_qt_offscreen_platform_plugin(TestNativeTextEditingLineEdit)

ladybird_test("TestWebContentViewInputMethod.cpp" UI/Qt LIBS LibWebView LibWeb LibGfx LibIPC LibURL Qt::Core Qt::Gui Qt::Widgets)
target_sources(TestWebContentViewInputMethod PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/InputMethodUtils.cpp" "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp")

if (NOT LINUX)
    return()
endif()

ladybird_test("TestNativeWindowContainerFocus.cpp" UI/Qt LIBS Qt::Core Qt::Gui Qt::Widgets)
target_sources(TestNativeWindowContainerFocus PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/NativeWindowContainer.cpp")
deploy_qt_offscreen_platform_plugin(TestNativeWindowContainerFocus)

ladybird_test("TestJavaScriptDialog.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS LibURL Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestJavaScriptDialog PRIVATE
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ChromeStyle.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/JavaScriptDialog.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/NativeTextEditingLineEdit.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
)
deploy_qt_offscreen_platform_plugin(TestJavaScriptDialog)

ladybird_test("TestWindowCloseCoordinator.cpp" UI/Qt LIBS Qt::Core)
target_sources(TestWindowCloseCoordinator PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/WindowCloseCoordinator.cpp")

ladybird_test("TestStringUtils.cpp" UI/Qt NAME TestQtStringUtils LIBS LibURL Qt::Core)
target_sources(TestQtStringUtils PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp")
