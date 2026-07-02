if (NOT LINUX)
    return()
endif()

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
if (NOT Qt6_FOUND)
    return()
endif()

ladybird_test("TestNativeWindowContainerFocus.cpp" UI/Qt LIBS Qt::Core Qt::Gui Qt::Widgets)
target_sources(TestNativeWindowContainerFocus PRIVATE "${LADYBIRD_SOURCE_DIR}/UI/Qt/NativeWindowContainer.cpp")
