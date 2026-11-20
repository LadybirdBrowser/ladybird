/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>

#include <QString>
#include <QWidget>

class QToolButton;

namespace Ladybird {

// A warning in a bar above the pages of a window.
class NotificationBar final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    struct Button {
        QString label;
        Function<void()> on_click;
    };

    // The bar has a close button if `on_close` is set.
    NotificationBar(QWidget& parent, QString const& message, Vector<Button>, Function<void()> on_close);

private:
    virtual bool event(QEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;

    void update_chrome_style();

    QWidget* m_icon_area { nullptr };
    QToolButton* m_close_button { nullptr };
};

}
