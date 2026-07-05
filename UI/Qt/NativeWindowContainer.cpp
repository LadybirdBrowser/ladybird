/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/NativeWindowContainer.h>

#include <QCoreApplication>
#include <QFocusEvent>
#include <QWidget>

namespace Ladybird {

namespace {

class FocusEventForwarder final : public QObject {
public:
    FocusEventForwarder(QWidget& host, QWidget& container)
        : QObject(&container)
        , m_host(host)
    {
    }

private:
    virtual bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
            QFocusEvent forwarded_event { event->type(), static_cast<QFocusEvent*>(event)->reason() };
            QCoreApplication::sendEvent(&m_host, &forwarded_event);
        }
        return false;
    }

    QWidget& m_host;
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

void install_native_window_container_focus_forwarding(QWidget& host, QWidget& container)
{
    container.installEventFilter(new FocusEventForwarder(host, container));
}

}
