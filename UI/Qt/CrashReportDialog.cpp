/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CrashReportStore.h>
#include <UI/Qt/CrashReportDialog.h>
#include <UI/Qt/CrashReportReviewWidget.h>

#include <QScrollArea>
#include <QVBoxLayout>

namespace Ladybird {

void CrashReportDialog::open_if_needed(QWidget& window)
{
    if (!WebView::CrashReportStore::the().has_pending_reports())
        return;

    auto* dialog = new CrashReportDialog(window);
    auto* review = new CrashReportReviewWidget(CrashReportReviewWidget::Mode::Dialog, dialog);
    if (auto result = review->open_report(); result.is_error()) {
        warnln("Could not open a crash report for review: {}", result.error());
        delete dialog;
        return;
    }
    review->on_close = [dialog] { dialog->close(); };

    auto* scroll_area = new QScrollArea(dialog);
    scroll_area->setFrameShape(QFrame::NoFrame);
    scroll_area->setWidgetResizable(true);
    scroll_area->setWidget(review);
    review->setContentsMargins(20, 20, 20, 20);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll_area);

    dialog->open();
}

CrashReportDialog::CrashReportDialog(QWidget& window)
    : QDialog(&window)
{
    setWindowTitle(tr("Crash Report"));
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(640, 720);
}

}
