/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CrashReportStore.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/CrashReportNotifier.h>
#include <UI/Qt/Tab.h>

#include <QApplication>
#include <QEvent>

namespace Ladybird {

static constexpr int CRASH_REPORT_POPOVER_WIDTH = 340;

// Only the newest report is offered. One still waiting to be shown is replaced, and stays pending on disk.
void CrashReportNotifier::offer(ByteString report_name)
{
    m_pending_report_name = move(report_name);
    show_pending();
}

// A popup of a window, such as this notifier's own popover, takes activation from the window while it is open.
static bool window_is_active(QWidget& window)
{
    if (window.isMinimized())
        return false;
    if (window.isActiveWindow())
        return true;
    auto* popup = QApplication::activePopupWidget();
    return popup && popup->parentWidget() && popup->parentWidget()->window() == &window;
}

// The report waits until a browser window is active and shows its toolbar, so the popover is shown where the user is.
void CrashReportNotifier::show_pending()
{
    if (!m_pending_report_name.has_value())
        return;

    auto* tab = Application::the().active_tab();
    auto* anchor = tab ? tab->hamburger_button() : nullptr;
    if (!anchor || !anchor->isVisible() || !window_is_active(*anchor->window()))
        return;

    // A newer report takes the place of one still on screen, wherever it is, which counts as offered.
    if (m_popover)
        m_popover->close();
    if (m_anchor)
        m_anchor->removeEventFilter(this);
    m_anchor = anchor;
    m_anchor->installEventFilter(this);

    auto report_name = m_pending_report_name.release_value();
    m_popover = new MessagePopover(anchor, CRASH_REPORT_POPOVER_WIDTH, tr("Ladybird flew off-course!"),
        tr("Part of Ladybird stopped unexpectedly. You can send us a crash report to help fix it."), tr("Dismiss"),
        tr("Review report"));
    m_popover->setAttribute(Qt::WA_DeleteOnClose);

    // The review marks the report as seen once it opens it. The popover is the report's one automatic prompt, so
    // however else it is left, the report is not offered again.
    m_popover->on_accept = [report_name] { Application::the().review_crash_report(report_name); };
    m_popover->on_dismiss = [report_name] {
        if (auto result = WebView::CrashReportStore::the().mark_seen(report_name); result.is_error())
            warnln("Could not mark crash report as seen: {}", result.error());
    };

    // The menu button's palette is transparent, so the popover takes its colors from the window instead.
    m_popover->update_chrome_style(anchor->window()->palette());
    m_popover->adjustSize();
    move_popover_below(*m_popover, *anchor);
    m_popover->show();
    move_popover_below(*m_popover, *anchor);
    m_popover->raise();
}

// A popup is a window of its own, so it stays on screen when the tab it was shown on goes out of view. It closes with
// it instead, and takes on the colors of the browser around it.
bool CrashReportNotifier::eventFilter(QObject* object, QEvent* event)
{
    if (object == m_anchor && m_popover) {
        if (event->type() == QEvent::Hide)
            m_popover->close();
        else if (event->type() == QEvent::PaletteChange)
            m_popover->update_chrome_style(m_anchor->window()->palette());
    }
    return QObject::eventFilter(object, event);
}

}
