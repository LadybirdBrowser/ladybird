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
    LIBS LibSync LibThreading Qt::Core
)
target_sources(TestEventLoopImplementation PRIVATE
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/EventLoopImplementationQt.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/EventLoopImplementationQtEventTarget.cpp"
)
set_target_properties(TestEventLoopImplementation PROPERTIES AUTOMOC ON)

find_package(Qt6 QUIET COMPONENTS Gui Widgets)
if (NOT Qt6_FOUND)
    return()
endif()

add_library(QtTestMain OBJECT
    QtTestMain.cpp
    "${LADYBIRD_SOURCE_DIR}/Libraries/LibTest/AssertionHandler.cpp"
)
target_link_libraries(QtTestMain PRIVATE AK LibCore LibTest Qt::Core Qt::Gui Qt::Widgets)

# QtTestMain runs its tests on Qt's offscreen platform plugin, and on Windows nothing deploys that plugin where a test
# binary can find it: The ladybird target's deploy step copies only the "windows" platform plugin into the bin/platforms
# directory — and for a binary in a deployed layout, that directory is the whole platform-plugin search path. So a
# QApplication there finds no platform plugin at all, which is fatal.
# NB: On Windows, Qt puts up a modal message box before that abort whenever the process has no console window — and a
# CI test process has none. So the test doesn't fail: It sits in the box until CTest's timeout kills it. Call this for
# every test that links QtTestMain, so the plugin sits next to the test binary, where Qt looks for it.
function(deploy_qt_offscreen_platform_plugin target)
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

ladybird_test("TestHiddenPageSizing.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestHiddenPageSizing PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/HiddenPageSizing.cpp")
deploy_qt_offscreen_platform_plugin(TestHiddenPageSizing)

if (NOT LINUX)
    return()
endif()

ladybird_test("TestNativeWindowContainerFocus.cpp" UI/Qt LIBS Qt::Core Qt::Gui Qt::Widgets)
target_sources(TestNativeWindowContainerFocus PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/NativeWindowContainer.cpp")

ladybird_test("TestJavaScriptDialog.cpp" UI/Qt
    CUSTOM_MAIN "$<TARGET_OBJECTS:QtTestMain>"
    LIBS LibURL Qt::Core Qt::Gui Qt::Widgets
)
target_sources(TestJavaScriptDialog PRIVATE
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/ChromeStyle.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/JavaScriptDialog.cpp"
    "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp"
)

ladybird_test("TestStringUtils.cpp" UI/Qt NAME TestQtStringUtils LIBS LibURL Qt::Core)
target_sources(TestQtStringUtils PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp")

ladybird_test("TestWebContentViewInputMethod.cpp" UI/Qt LIBS LibWebView LibWeb LibGfx LibIPC LibURL Qt::Core Qt::Gui Qt::Widgets)
target_sources(TestWebContentViewInputMethod PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/InputMethodUtils.cpp" "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp")
