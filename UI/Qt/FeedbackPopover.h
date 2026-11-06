/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/kmalloc.h>
#include <LibCore/BrowserVersion.h>
#include <UI/Qt/Popover.h>

class QFrame;
class QLabel;
class QPushButton;

namespace Ladybird {

// Builds that are still being tested ask for feedback from the toolbar. A final release does not.
static constexpr bool has_feedback_button = !BROWSER_IS_FINAL_RELEASE;

class FeedbackPopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit FeedbackPopover(QWidget* parent);

    void update_chrome_style(QPalette const&);

    // Without a host, the page is not a website that can be reported, and the panel does not offer to.
    void set_reportable_host(QString const& host);
    void set_crash_report_count(size_t);

    Function<void()> on_report_site;
    Function<void()> on_report_bug;
    Function<void()> on_review_crash_reports;

private:
    QPushButton* m_report_site_row { nullptr };
    QLabel* m_host_label { nullptr };
    QLabel* m_external_link_icon { nullptr };
    QWidget* m_crash_reports_section { nullptr };
    QLabel* m_crash_reports_dot { nullptr };
    QLabel* m_crash_reports_count { nullptr };
    QLabel* m_crash_reports_chevron { nullptr };
};

}
