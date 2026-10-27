/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibWebView/CrashReportStore.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/CrashReportNotifier.h>
#include <UI/Qt/LadybirdButton.h>
#include <UI/Qt/Tab.h>

#include <QApplication>
#include <QEvent>
#include <QGuiApplication>

namespace Ladybird {

static constexpr int CRASH_REPORT_POPOVER_WIDTH = 340;

// Only the newest report is offered. One still waiting to be shown is replaced, and stays pending on disk.
void CrashReportNotifier::offer(ByteString report_name)
{
    m_pending_report_name = move(report_name);

    if (!exchange(m_is_listening_for_activation, true)) {
        QObject::connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive)
                show_pending();
        });
    }
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
// It also waits for the application itself to be active. Qt closes a popup on the click that activates an inactive
// application, which would dismiss the popover before its buttons could be used.
void CrashReportNotifier::show_pending()
{
    if (!m_pending_report_name.has_value() || QGuiApplication::applicationState() != Qt::ApplicationActive)
        return;

    auto* window = Application::the().active_window_if_any();
    QWidget* anchor = window ? &window->ladybird_button() : nullptr;
    if (!anchor || !anchor->isVisible() || !window_is_active(*anchor->window()))
        return;

    // A newer report takes the place of one still on screen, wherever it is, which counts as offered.
    if (m_popover) {
        m_is_replacing_popover = true;
        m_popover->close();
        m_is_replacing_popover = false;
    }
    if (m_anchor)
        m_anchor->removeEventFilter(this);
    m_anchor = anchor;
    m_anchor->installEventFilter(this);

    auto report_name = m_pending_report_name.release_value();
    m_popover = new MessagePopover(anchor, CRASH_REPORT_POPOVER_WIDTH, tr("Part of Ladybird stopped unexpectedly"),
        tr("You can send us a crash report to help fix it."), tr("Dismiss"),
        tr("Review report"));
    m_popover->setAttribute(Qt::WA_DeleteOnClose);

    // The review marks the report as seen once it opens it. The popover is the report's one automatic prompt, so
    // however else it is left, the report is not offered again. Leaving it also declines the reports still waiting,
    // except when a newer report takes its place, which is about to be offered itself.
    m_popover->on_accept = [report_name] { Application::the().review_crash_report(report_name); };
    m_popover->on_dismiss = [this, report_name] {
        auto& store = WebView::CrashReportStore::the();
        auto result = m_is_replacing_popover ? store.mark_seen(report_name) : store.mark_all_seen();
        if (result.is_error())
            warnln("Could not mark crash reports as seen: {}", result.error());
    };

    // The menu button's palette is transparent, so the popover takes its colors from the window instead.
    m_popover->update_chrome_style(anchor->window()->palette());
    m_popover->adjustSize();
    move_popover_below(*m_popover, *anchor, PopoverAlignment::Leading);
    m_popover->show();
    move_popover_below(*m_popover, *anchor, PopoverAlignment::Leading);
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
