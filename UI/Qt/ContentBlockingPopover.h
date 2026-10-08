/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWebView/ContentBlocking.h>
#include <UI/Qt/Popover.h>

class QCheckBox;
class QIcon;
class QLabel;
class QKeyEvent;
class QMouseEvent;

namespace Ladybird {

class ContentBlockingPopover final : public Popover {
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit ContentBlockingPopover(QWidget* parent);

    void set_snapshot(WebView::ContentBlockingSnapshot const&);
    void set_favicon(QIcon const&);
    void update_chrome_style(QPalette const&);
    void show_error(QString const&);
    void focus_switch();

    Function<void(bool)> on_toggle;
    Function<void()> on_keyboard_dismiss;

private:
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    void update_size();

    QLabel* m_favicon { nullptr };
    QLabel* m_site { nullptr };
    QLabel* m_count { nullptr };
    QLabel* m_note { nullptr };
    QLabel* m_error { nullptr };
    QCheckBox* m_switch { nullptr };
};

}
