/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Permissions/Permissions.h>
#include <LibWebView/ContentBlocking.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/ViewImplementation.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Popover.h>

class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace Ladybird {

// What Ladybird knows about the site a page is from, and the settings that apply to it.
class SiteSettingsPopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit SiteSettingsPopover(QWidget* parent);

    struct Site {
        String host;
        bool is_secure { false };
        QIcon favicon;
        WebView::ContentBlockingSnapshot content_blocking;
        Vector<WebView::SitePermissionRow> permissions;
    };
    void set_site(Site);
    void set_content_blocking(WebView::ContentBlockingSnapshot const&);
    void set_permissions(Vector<WebView::SitePermissionRow> const&);
    void update_chrome_style(QPalette const&);

    Function<void(bool)> on_content_blocking_toggle;
    // Changes one of the site's permissions, to the decision given, or back to asking when there is none.
    Function<void(Web::Permissions::PermissionName, Optional<WebView::PermissionDecision>)> on_permission_change;
    Function<void()> on_reset_permissions;
    Function<void()> on_open_all_site_settings;
    Function<void()> on_report_broken_site;
    Function<void()> on_open_filter_lists;

private:
    void update_autoplay();
    void update_reset_link();
    void update_icons();
    void show_blocked_resources(bool);
    void fit_to_page();

    struct PermissionRow {
        Web::Permissions::PermissionName name;
        QLabel* icon { nullptr };
        QPushButton* button { nullptr };
    };

    String m_host;
    QWidget* m_site_page { nullptr };
    QWidget* m_blocked_page { nullptr };
    QLabel* m_favicon { nullptr };
    QLabel* m_host_label { nullptr };
    QLabel* m_security_label { nullptr };
    QWidget* m_permission_rows { nullptr };
    Vector<PermissionRow> m_permissions;
    bool m_has_decided_permissions { false };
    QLabel* m_autoplay_icon { nullptr };
    QPushButton* m_autoplay_button { nullptr };
    QPushButton* m_reset_permissions_link { nullptr };
    QLabel* m_content_blocking_icon { nullptr };
    PopoverSwitch* m_content_blocking_switch { nullptr };
    QPushButton* m_blocked_row { nullptr };
    QLabel* m_blocked_row_chevron { nullptr };
    QToolButton* m_back_button { nullptr };
    QLabel* m_blocked_count { nullptr };
    QLabel* m_blocked_page_count { nullptr };
    QVBoxLayout* m_blocked_list { nullptr };
};

}
