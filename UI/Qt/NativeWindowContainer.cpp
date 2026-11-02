/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/kmalloc.h>
#include <UI/Qt/NativeWindowContainer.h>

#include <QEvent>
#include <QWidget>
#include <QWindow>

namespace Ladybird {

namespace {

class ContainerPaintFilter final : public QObject {
public:
    AK_ALLOC_WITH_KMALLOC;

    using QObject::QObject;

private:
    virtual bool eventFilter(QObject*, QEvent* event) override
    {
        // NB: QWindowContainer clears its whole area to transparent on platforms that may stack native child windows
        //     below their parent. Ours are always above it, and the clear would only hide the host's own painting
        //     wherever the native window has nothing on screen yet.
        return event->type() == QEvent::Paint;
    }
};

}

QWidget* create_native_window_container(QWindow& native_window, QWidget& host)
{
    // NB: The host takes all pointer input and drags, as the receiver of a press keeps getting events while it is
    //     dragged out of the browser window, or while the container hides. The window must not take the focus either:
    //     On X11, a window that is transparent for input doesn't ask for key events, and X doesn't pass them on past
    //     the window with the input focus.
    native_window.setFlag(Qt::WindowTransparentForInput);
    native_window.setFlag(Qt::WindowDoesNotAcceptFocus);

    auto* container = QWidget::createWindowContainer(&native_window, &host);
    container->setAttribute(Qt::WA_TransparentForMouseEvents);
    container->installEventFilter(new ContainerPaintFilter(container));
    return container;
}

}
