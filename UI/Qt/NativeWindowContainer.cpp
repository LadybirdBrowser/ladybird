/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/NativeWindowContainer.h>

#include <QCoreApplication>
#include <QFocusEvent>
#include <QWidget>
#include <QWindow>

namespace Ladybird {

namespace {

class EventForwarder final : public QObject {
public:
    EventForwarder(QWidget& host, QWidget& container)
        : QObject(&container)
        , m_host(host)
        , m_container(container)
    {
    }

private:
    virtual bool eventFilter(QObject* watched, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            if (watched == &m_container) {
                QFocusEvent forwarded_event { event->type(), static_cast<QFocusEvent*>(event)->reason() };
                QCoreApplication::sendEvent(&m_host, &forwarded_event);
            }
            break;

        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            // Keyboard input belongs to the host, not the container. Forwarding it from an event filter also preempts
            // QWidget's Tab focus navigation, which would otherwise consume Tab before the host sees it.
            if (watched == &m_container)
                return QCoreApplication::sendEvent(&m_host, event);
            break;

        case QEvent::MouseButtonPress:
            if (m_host.focusProxy() == &m_container)
                m_container.setFocus(Qt::MouseFocusReason);
            break;

        default:
            break;
        }

        return false;
    }

    QWidget& m_host;
    QWidget& m_container;
};

}

void set_native_window_container_visible(QWidget& host, QWidget& container, bool visible)
{
    if (container.isVisible() == visible)
        return;

    if (visible) {
        bool host_had_focus = host.hasFocus();
        container.setGeometry(host.rect());
        container.show();
        host.setFocusProxy(&container);
        if (host_had_focus)
            container.setFocus();
    } else {
        bool container_had_focus = container.hasFocus();
        host.setFocusProxy(nullptr);
        container.hide();
        if (container_had_focus)
            host.setFocus();
    }
}

void install_native_window_container_event_forwarding(QWidget& host, QWindow& native_window, QWidget& container)
{
    auto* forwarder = new EventForwarder(host, container);
    native_window.installEventFilter(forwarder);
    container.installEventFilter(forwarder);
}

}
