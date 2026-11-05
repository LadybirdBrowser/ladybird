/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibTest/TestCase.h>

#include <QApplication>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>
#include <QWidget>
#include <QWindow>
#include <UI/Qt/NativeWindowContainer.h>

namespace {

QApplication& application()
{
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    VERIFY(app);
    return *app;
}

struct TestWindow {
    TestWindow()
        : host(*new QWidget(&window))
        , sibling(*new QWidget(&window))
        , native_window(*new QWindow)
        , container(*Ladybird::create_native_window_container(native_window, host))
    {
        window.resize(200, 100);
        host.setGeometry(0, 0, 100, 100);
        sibling.setGeometry(100, 0, 100, 100);
        container.setGeometry(host.rect());

        host.setFocusPolicy(Qt::StrongFocus);
        sibling.setFocusPolicy(Qt::StrongFocus);
        container.hide();

        window.show();
        window.activateWindow();
        QApplication::processEvents();
    }

    QWidget window;
    QWidget& host;
    QWidget& sibling;
    QWindow& native_window;
    QWidget& container;
};

class EventRecorder final : public QObject {
public:
    Vector<QEvent::Type> received;

private:
    virtual bool eventFilter(QObject*, QEvent* event) override
    {
        received.append(event->type());
        return false;
    }
};

}

static void send_mouse_event(QWidget& window, QEvent::Type type, Qt::MouseButtons buttons)
{
    QPointF const position { 50, 50 };
    auto const global_position = window.mapToGlobal(position);
    QMouseEvent event { type, position, position, global_position, Qt::LeftButton, buttons, Qt::NoModifier };
    QCoreApplication::sendEvent(window.windowHandle(), &event);
}

TEST_CASE(native_window_takes_no_input)
{
    application();
    TestWindow widgets;

    EXPECT(widgets.native_window.flags().testFlag(Qt::WindowTransparentForInput));
    EXPECT(widgets.native_window.flags().testFlag(Qt::WindowDoesNotAcceptFocus));
}

TEST_CASE(host_keeps_the_keyboard_focus)
{
    application();
    TestWindow widgets;

    widgets.host.setFocus();
    QApplication::processEvents();
    EXPECT_EQ(QApplication::focusWidget(), &widgets.host);

    widgets.container.show();
    EXPECT_EQ(QApplication::focusWidget(), &widgets.host);

    widgets.container.hide();
    EXPECT_EQ(QApplication::focusWidget(), &widgets.host);
}

TEST_CASE(focus_is_not_stolen_from_other_widgets)
{
    application();
    TestWindow widgets;

    widgets.sibling.setFocus();
    QApplication::processEvents();
    EXPECT_EQ(QApplication::focusWidget(), &widgets.sibling);

    widgets.container.show();
    EXPECT_EQ(QApplication::focusWidget(), &widgets.sibling);

    widgets.container.hide();
    EXPECT_EQ(QApplication::focusWidget(), &widgets.sibling);
}

TEST_CASE(pointer_input_goes_past_the_container)
{
    application();
    TestWindow widgets;

    widgets.container.show();
    EXPECT_EQ(widgets.window.childAt(50, 50), &widgets.host);
}

TEST_CASE(press_continues_while_the_container_hides)
{
    application();
    TestWindow widgets;

    EventRecorder recorder;
    widgets.host.installEventFilter(&recorder);
    widgets.container.show();

    send_mouse_event(widgets.window, QEvent::MouseButtonPress, Qt::LeftButton);
    EXPECT(recorder.received.contains_slow(QEvent::MouseButtonPress));

    widgets.container.hide();

    send_mouse_event(widgets.window, QEvent::MouseButtonRelease, Qt::NoButton);
    EXPECT(recorder.received.contains_slow(QEvent::MouseButtonRelease));
}

TEST_CASE(drags_go_past_the_container)
{
    application();
    TestWindow widgets;

    EventRecorder recorder;
    widgets.host.installEventFilter(&recorder);
    widgets.host.setAcceptDrops(true);
    widgets.container.show();

    QMimeData mime_data;
    mime_data.setUrls({ QUrl { "file:///dragged" } });

    // Qt 6.12 added a QPointF constructor, and deprecated the QPoint one.
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    QPointF drag_position { 50, 50 };
#else
    QPoint drag_position { 50, 50 };
#endif
    QDragEnterEvent drag_enter { drag_position, Qt::CopyAction, &mime_data, Qt::LeftButton, Qt::NoModifier };
    QCoreApplication::sendEvent(widgets.window.windowHandle(), &drag_enter);
    EXPECT(recorder.received.contains_slow(QEvent::DragEnter));

    QDragLeaveEvent drag_leave;
    QCoreApplication::sendEvent(widgets.window.windowHandle(), &drag_leave);
}
