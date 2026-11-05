if (NOT Qt6_FOUND)
    return()
endif()

add_library(QtTestMain OBJECT
    QtTestMain.cpp
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibTest/AssertionHandler.cpp"
)
target_link_libraries(QtTestMain PRIVATE AK LibCore LibTest Qt::Core Qt::Gui Qt::Widgets)
ladybird_test_suite(UI/Qt NO_TEST CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>")
target_link_libraries(TestUIQt PRIVATE LibCore LibFileSystem LibGfx LibIPC LibURL LibWeb LibWebCommon LibWebView ladybird_impl)

add_library(QtEventLoopTestMain OBJECT
    QtEventLoopTestMain.cpp
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibTest/AssertionHandler.cpp"
)
target_link_libraries(QtEventLoopTestMain PRIVATE AK LibCore LibTest Qt::Core)

ladybird_test("TestEventLoopImplementation.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtEventLoopTestMain>"
    LIBS LibThreading ladybird_impl
)

ladybird_test("TestFullscreenDebounce.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtEventLoopTestMain>"
    LIBS LibThreading ladybird_impl
)

set(TEST_SOURCES
    TestBackingStoreRelease.cpp
    TestChromeIcons.cpp
    TestHiddenPageSizing.cpp
    TestJavaScriptDialog.cpp
    TestNativeTextEditingLineEdit.cpp
    TestNativeWindowContainerFocus.cpp
    TestPerformanceMonitorWidget.cpp
    TestPermissionPromptPopover.cpp
    TestProcessManagerWindow.cpp
    TestScreenshotSelectionOverlay.cpp
    TestScreenshotViewer.cpp
    TestSelectDropdown.cpp
    TestSiteSettingsPopover.cpp
    TestStringUtils.cpp
    TestWebCompatReportReview.cpp
    TestWebContentViewInputMethod.cpp
    TestWindowCloseCoordinator.cpp
)

# Crash reports are only saved on macOS and Linux.
if (APPLE OR LINUX)
    list(APPEND TEST_SOURCES
        TestCrashReportReviewWidget.cpp
    )
endif()

target_sources(TestUIQt PRIVATE ${TEST_SOURCES})
set_property(SOURCE TestProcessManagerWindow.cpp APPEND PROPERTY COMPILE_DEFINITIONS TEST_RESOURCE_ROOT="${LADYBIRD_SOURCE_DIR}/Base/res")

# QtTestMain runs on the offscreen platform plugin. Static Qt needs it imported explicitly, and Windows needs it
# copied alongside the default platform plugin deployed by the ladybird target.
ladybird_import_qt_plugin(TestUIQt QOffscreenIntegrationPlugin)
if (TARGET Qt6::QOffscreenIntegrationPlugin)
    add_test(NAME TestUIQt COMMAND TestUIQt WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    if (WIN32)
        add_custom_command(TARGET TestUIQt POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:TestUIQt>/platforms"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
                "$<TARGET_FILE_DIR:TestUIQt>/platforms/"
            VERBATIM
        )
    endif()
endif()

if (APPLE)
    add_library(NativeTextEditingTestMain OBJECT
        NativeTextEditingTestMain.mm
        "${LADYBIRD_SOURCE_DIR}/Libraries/LibTest/AssertionHandler.cpp"
    )
    target_link_libraries(NativeTextEditingTestMain PRIVATE AK LibTest "-framework AppKit")
    ladybird_test("TestNativeTextEditingBindings.mm" UI/Qt
        NO_TEST
        CUSTOM_MAIN "$<TARGET_OBJECTS:NativeTextEditingTestMain>"
        LIBS LibWebCommon Qt::Widgets "-framework AppKit"
    )
    foreach(mode default custom)
        ladybird_python_test(run-native-text-editing-tests.py
            NAME TestNativeTextEditingBindings-${mode}
            ARGS $<TARGET_FILE:TestNativeTextEditingBindings> ${mode})
    endforeach()
endif()
