/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>
#include <LibWebView/ContentBlocking.h>

#include <QIcon>
#include <QToolButton>

namespace Ladybird {

class ContentBlockingButton final : public QToolButton {
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit ContentBlockingButton(QWidget* parent);

    void set_snapshot(WebView::ContentBlockingSnapshot const&);
    void set_show_count_badge(bool);

    QString badge_text() const;

private:
    virtual void changeEvent(QEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;
    virtual QSize sizeHint() const override { return { 34, 34 }; }

    QIcon m_icon;
    WebView::ContentBlockingSnapshot m_snapshot;
    bool m_show_count_badge { false };
};

}
