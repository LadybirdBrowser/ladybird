/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <QApplication>
#include <QMouseEvent>
#include <QWidget>
#include <QWindow>
#include <UI/Qt/NativeWindowContainer.h>

namespace {

QApplication& application()
{
    static struct ForceOffscreenPlatform {
        ForceOffscreenPlatform() { qputenv("QT_QPA_PLATFORM", "offscreen"); }
    } force_offscreen_platform;
    static int argc = 1;
    static char program_name[] = "TestNativeWindowContainerFocus";
    static char* argv[] = { program_name, nullptr };
    static QApplication app(argc, argv);
    return app;
}

struct TestWindow {
    TestWindow()
        : host(*new QWidget(&window))
        , container(*new QWidget(&host))
        , sibling(*new QWidget(&window))
    {
        host.setFocusPolicy(Qt::StrongFocus);
        sibling.setFocusPolicy(Qt::StrongFocus);
        container.setFocusPolicy(Qt::StrongFocus);
        container.hide();

        window.show();
        window.activateWindow();
        QApplication::processEvents();
    }

    QWidget window;
    QWidget& host;
    QWidget& container;
    QWidget& sibling;
};

class FocusEventRecorder final : public QObject {
public:
    QEvent::Type last_focus_event { QEvent::None };

private:
    virtual bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
            last_focus_event = event->type();
        return false;
    }
};

}

TEST_CASE(focus_proxy_follows_container_visibility)
{
    application();
    TestWindow widgets;

    EXPECT(!widgets.host.focusProxy());

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, true);
    EXPECT(widgets.container.isVisible());
    EXPECT_EQ(widgets.host.focusProxy(), &widgets.container);
    EXPECT_EQ(widgets.container.geometry(), widgets.host.rect());

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, false);
    EXPECT(!widgets.container.isVisible());
    EXPECT(!widgets.host.focusProxy());
}

TEST_CASE(keyboard_focus_moves_with_the_container)
{
    application();
    TestWindow widgets;

    widgets.host.setFocus();
    QApplication::processEvents();
    EXPECT_EQ(QApplication::focusWidget(), &widgets.host);

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, true);
    EXPECT_EQ(QApplication::focusWidget(), &widgets.container);

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, false);
    EXPECT_EQ(QApplication::focusWidget(), &widgets.host);
}

TEST_CASE(focus_is_not_stolen_from_other_widgets)
{
    application();
    TestWindow widgets;

    widgets.sibling.setFocus();
    QApplication::processEvents();
    EXPECT_EQ(QApplication::focusWidget(), &widgets.sibling);

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, true);
    EXPECT_EQ(widgets.host.focusProxy(), &widgets.container);
    EXPECT_EQ(QApplication::focusWidget(), &widgets.sibling);

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, false);
    EXPECT_EQ(QApplication::focusWidget(), &widgets.sibling);
}

TEST_CASE(container_focus_events_are_forwarded_to_the_host)
{
    application();
    TestWindow widgets;

    QWindow native_window;
    Ladybird::install_native_window_container_focus_forwarding(widgets.host, native_window, widgets.container);

    FocusEventRecorder recorder;
    widgets.host.installEventFilter(&recorder);

    widgets.host.setFocus();
    QApplication::processEvents();

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, true);
    EXPECT_EQ(QApplication::focusWidget(), &widgets.container);
    EXPECT_EQ(recorder.last_focus_event, QEvent::FocusIn);

    widgets.sibling.setFocus();
    EXPECT_EQ(recorder.last_focus_event, QEvent::FocusOut);

    widgets.container.setFocus();
    EXPECT_EQ(recorder.last_focus_event, QEvent::FocusIn);

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, false);
    EXPECT_EQ(QApplication::focusWidget(), &widgets.host);
    EXPECT_EQ(recorder.last_focus_event, QEvent::FocusIn);
}

TEST_CASE(native_window_mouse_press_focuses_the_container)
{
    application();
    TestWindow widgets;

    QWindow native_window;
    Ladybird::install_native_window_container_focus_forwarding(widgets.host, native_window, widgets.container);

    FocusEventRecorder recorder;
    widgets.host.installEventFilter(&recorder);

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, true);
    widgets.sibling.setFocus();
    QApplication::processEvents();
    EXPECT_EQ(QApplication::focusWidget(), &widgets.sibling);

    recorder.last_focus_event = QEvent::None;

    QMouseEvent mouse_press {
        QEvent::MouseButtonPress,
        QPointF { 1, 1 },
        QPointF { 1, 1 },
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::NoModifier
    };
    QApplication::sendEvent(&native_window, &mouse_press);

    EXPECT_EQ(QApplication::focusWidget(), &widgets.container);
    EXPECT_EQ(recorder.last_focus_event, QEvent::FocusIn);
}

TEST_CASE(visibility_changes_are_idempotent)
{
    application();
    TestWindow widgets;

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, false);
    EXPECT(!widgets.container.isVisible());
    EXPECT(!widgets.host.focusProxy());

    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, true);
    Ladybird::set_native_window_container_visible(widgets.host, widgets.container, true);
    EXPECT(widgets.container.isVisible());
    EXPECT_EQ(widgets.host.focusProxy(), &widgets.container);
}
