/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/TabPerformanceStats.h>
#include <QStringList>
#include <QWidget>

namespace Ladybird {

class PerformanceMonitorWidget final : public QWidget {
public:
    explicit PerformanceMonitorWidget(QWidget* parent);
    void set_stats(WebView::TabPerformanceStats const&);
    virtual QSize sizeHint() const override;

private:
    virtual void paintEvent(QPaintEvent*) override;
    QStringList text_segments() const;
    WebView::TabPerformanceStats m_stats;
};

}
