/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/NativeWindowContainer.h>

#include <QWidget>

namespace Ladybird {

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

}
