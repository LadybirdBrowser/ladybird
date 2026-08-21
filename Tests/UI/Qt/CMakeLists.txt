if (NOT LINUX)
    return()
endif()

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
if (NOT Qt6_FOUND)
    return()
endif()

ladybird_test("TestNativeWindowContainerFocus.cpp" UI/Qt LIBS Qt::Core Qt::Gui Qt::Widgets)
target_sources(TestNativeWindowContainerFocus PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/NativeWindowContainer.cpp")

ladybird_test("TestStringUtils.cpp" UI/Qt NAME TestQtStringUtils LIBS LibURL Qt::Core)
target_sources(TestQtStringUtils PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp")

ladybird_test("TestWebContentViewInputMethod.cpp" UI/Qt LIBS LibWebView LibWeb LibGfx LibIPC LibURL Qt::Core Qt::Gui Qt::Widgets)
target_sources(TestWebContentViewInputMethod PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/InputMethodUtils.cpp" "${LADYBIRD_SOURCE_DIR}/UI/Qt/StringUtils.cpp")
