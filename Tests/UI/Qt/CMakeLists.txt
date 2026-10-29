find_package(Qt6 QUIET COMPONENTS Core)
if (NOT Qt6_FOUND)
    return()
endif()

find_package(Qt6 QUIET COMPONENTS Gui Widgets)
if (Qt6_FOUND)
    add_library(QtTestMain OBJECT
        QtTestMain.cpp
        "${LADYBIRD_SOURCE_DIR}/Libraries/LibTest/AssertionHandler.cpp"
    )
    target_link_libraries(QtTestMain PRIVATE AK LibCore LibTest Qt::Core Qt::Gui Qt::Widgets)
    ladybird_test_suite(UI/Qt CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>")
    target_link_libraries(TestUIQt PRIVATE LibCore LibFileSystem LibGfx LibIPC LibURL LibWeb LibWebCommon LibWebView Qt::Core Qt::Gui Qt::Widgets)
else()
    ladybird_test_suite(UI/Qt)
endif()

target_sources(TestUIQt PRIVATE TestBackingStoreRelease.cpp)

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

if (NOT Qt6_FOUND)
    return()
endif()

set(TEST_SOURCES
    TestChromeIcons.cpp
    TestHiddenPageSizing.cpp
    TestNativeTextEditingLineEdit.cpp
    TestProcessManagerWindow.cpp
    TestScreenshotSelectionOverlay.cpp
    TestScreenshotViewer.cpp
    TestSelectDropdown.cpp
    TestWebCompatReportReview.cpp
    TestWebContentViewInputMethod.cpp
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibWebView/ProcessMonitor.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ChromeStyle.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/HiddenPageSizing.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/Icon.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/InputMethodUtils.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/NativeTextEditingLineEdit.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/Popover.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ProcessManagerWindow.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ReportReview.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ScreenshotSelectionOverlay.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ScreenshotViewer.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/SelectDropdown.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
    # The chrome's styles take their colors from the theme, which follows the system's appearance.
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/SystemAppearance.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/Theme.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/WebCompatReportReview.cpp"
)

# Crash reports are only saved on macOS and Linux.
if (APPLE OR LINUX)
    list(APPEND TEST_SOURCES
        TestCrashReportReviewWidget.cpp
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/CrashReportReviewWidget.cpp"
    )
endif()

if (APPLE)
    list(APPEND TEST_SOURCES
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/SystemAppearanceMac.mm"
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/NativeTextEditingBindings.mm"
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/MacWindowFocus.mm"
    )
    target_link_libraries(TestUIQt PRIVATE "-framework AppKit")
endif()

if (LINUX)
    list(APPEND TEST_SOURCES
        TestJavaScriptDialog.cpp
        TestNativeWindowContainerFocus.cpp
        TestPermissionPromptPopover.cpp
        TestStringUtils.cpp
        TestWindowCloseCoordinator.cpp
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/NativeWindowContainer.cpp"
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/JavaScriptDialog.cpp"
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/PermissionPromptPopover.cpp"
        "${LADYBIRD_SOURCE_DIR}/UI/Qt/WindowCloseCoordinator.cpp"
    )
endif()

target_sources(TestUIQt PRIVATE ${TEST_SOURCES})
set_property(SOURCE TestProcessManagerWindow.cpp APPEND PROPERTY COMPILE_DEFINITIONS TEST_RESOURCE_ROOT="${LADYBIRD_SOURCE_DIR}/Base/res")
set_target_properties(TestUIQt PROPERTIES AUTOMOC ON)

# QtTestMain runs on the offscreen platform plugin. Static Qt needs it imported explicitly, and Windows needs it
# copied alongside the default platform plugin deployed by the ladybird target.
qt_import_plugins(TestUIQt INCLUDE Qt6::QOffscreenIntegrationPlugin)
if (WIN32)
    add_custom_command(TARGET TestUIQt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TestUIQt>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
            "$<TARGET_FILE_DIR:TestUIQt>/platforms/"
        VERBATIM
    )
endif()
