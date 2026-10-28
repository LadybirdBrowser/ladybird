/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/SiteSettingsPopover.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/Theme.h>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int POPOVER_WIDTH = 344;
static constexpr int ROW_HEIGHT = 36;
static constexpr int BLOCKED_RESOURCE_ROW_HEIGHT = 26;

static void add_separator(QWidget& parent, QVBoxLayout& layout, int top_margin, int bottom_margin)
{
    layout.addSpacing(top_margin);
    auto* separator = new QFrame(&parent);
    separator->setObjectName("LadybirdPopoverSeparator");
    separator->setFixedHeight(1);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(10, 0, 10, 0);
    row->addWidget(separator);
    layout.addLayout(row);
    layout.addSpacing(bottom_margin);
}

static QLabel* create_icon(QWidget& parent)
{
    auto* label = new QLabel(&parent);
    label->setFixedSize(16, 16);
    return label;
}

static QPushButton* create_link(QWidget& parent, QString const& text)
{
    auto* link = new QPushButton(text, &parent);
    link->setObjectName("LadybirdPopoverLink");
    link->setCursor(Qt::PointingHandCursor);
    // NB: The platform style lays out a push button by a frame of its own rather than by the button, which would
    //     make it overlap its neighbors.
    link->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    return link;
}

// The popover shows the site, and the settings that apply to it. Its content blocking leads on to what was blocked on
// the page, shown in place of the site until the popover goes back to it.
static QString autoplay_policy_name(Web::HTML::AutoplayPolicy policy)
{
    switch (policy) {
    case Web::HTML::AutoplayPolicy::AllowAudioAndVideo:
        return QStringLiteral("Allow");
    case Web::HTML::AutoplayPolicy::BlockAudio:
        return QStringLiteral("Block audio");
    case Web::HTML::AutoplayPolicy::BlockAudioAndVideo:
        return QStringLiteral("Block");
    }
    VERIFY_NOT_REACHED();
}

static QString permission_label(Web::Permissions::PermissionName name)
{
    switch (name) {
    case Web::Permissions::PermissionName::Geolocation:
        return QStringLiteral("Location");
    case Web::Permissions::PermissionName::Microphone:
        return QStringLiteral("Microphone");
    }
    VERIFY_NOT_REACHED();
}

static ChromeIcon permission_icon(Web::Permissions::PermissionName name)
{
    switch (name) {
    case Web::Permissions::PermissionName::Geolocation:
        return ChromeIcon::Location;
    case Web::Permissions::PermissionName::Microphone:
        return ChromeIcon::Microphone;
    }
    VERIFY_NOT_REACHED();
}

// The value a permission row shows for the site's decision; a site with none is asked.
static QString permission_decision_name(Optional<WebView::PermissionDecision> decision)
{
    if (!decision.has_value())
        return QStringLiteral("Ask");
    switch (*decision) {
    case WebView::PermissionDecision::Allow:
        return QStringLiteral("Allow");
    case WebView::PermissionDecision::AllowOnce:
        return QStringLiteral("Allow this time");
    case WebView::PermissionDecision::Block:
        return QStringLiteral("Block");
    }
    VERIFY_NOT_REACHED();
}

static QPushButton* create_value_button(QWidget& parent)
{
    auto* button = new QPushButton(&parent);
    button->setObjectName("LadybirdPopoverValueButton");
    button->setCursor(Qt::PointingHandCursor);
    button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    // NB: The chevron follows the value.
    button->setLayoutDirection(Qt::RightToLeft);
    button->setIconSize({ 12, 12 });
    return button;
}

SiteSettingsPopover::SiteSettingsPopover(QWidget* parent)
    : Popover(parent)
{
    setAccessibleName("Site settings");
    card().setObjectName("LadybirdSiteSettingsPopover");
    card().setFixedWidth(POPOVER_WIDTH);

    auto* pages = new QVBoxLayout(&card());
    pages->setContentsMargins(0, 0, 0, 0);
    pages->setSpacing(0);

    // The site.
    m_site_page = new QWidget(&card());
    auto* layout = new QVBoxLayout(m_site_page);
    layout->setContentsMargins(6, 14, 6, 8);
    layout->setSpacing(0);

    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(10, 0, 10, 0);
    heading->setSpacing(10);
    m_favicon = new QLabel(m_site_page);
    m_favicon->setFixedSize(28, 28);
    m_favicon->setAlignment(Qt::AlignCenter);
    m_favicon->setObjectName("LadybirdSiteSettingsFavicon");
    heading->addWidget(m_favicon);
    auto* heading_text = new QVBoxLayout;
    heading_text->setSpacing(2);
    m_host_label = new QLabel(m_site_page);
    m_host_label->setObjectName("LadybirdSiteSettingsHost");
    m_host_label->setTextFormat(Qt::PlainText);
    heading_text->addWidget(m_host_label);
    m_security_label = new QLabel(m_site_page);
    m_security_label->setObjectName("LadybirdSiteSettingsSecurity");
    m_security_label->setTextFormat(Qt::PlainText);
    heading_text->addWidget(m_security_label);
    heading->addLayout(heading_text, 1);
    layout->addLayout(heading);

    add_separator(*m_site_page, *layout, 12, 6);

    // The permissions the user decided for the site, or the page asked for. The rows are made for each site shown.
    m_permission_rows = new QWidget(m_site_page);
    auto* permission_rows_layout = new QVBoxLayout(m_permission_rows);
    permission_rows_layout->setContentsMargins(0, 0, 0, 0);
    permission_rows_layout->setSpacing(0);
    layout->addWidget(m_permission_rows);

    // Whether media may play on its own.
    auto* autoplay_row = new QHBoxLayout;
    autoplay_row->setContentsMargins(10, 0, 10, 0);
    autoplay_row->setSpacing(10);
    m_autoplay_icon = create_icon(*m_site_page);
    autoplay_row->addWidget(m_autoplay_icon);
    auto* autoplay_label = new QLabel("Autoplay", m_site_page);
    autoplay_label->setFixedHeight(ROW_HEIGHT);
    autoplay_row->addWidget(autoplay_label, 1);
    m_autoplay_button = create_value_button(*m_site_page);
    QObject::connect(m_autoplay_button, &QPushButton::clicked, this, [this] {
        // A site is either allowed to autoplay, or follows the default, which the menu names so that the choice it shows
        // on the button is always one of those in the menu.
        auto const& autoplay = WebView::Application::settings().autoplay_settings();
        auto is_allowed_on_site = autoplay.site_filters.contains(m_host);
        QMenu menu(this);
        auto* allow = menu.addAction("Allow");
        allow->setCheckable(true);
        allow->setChecked(is_allowed_on_site);
        auto* use_default = menu.addAction(qformatted("Default ({})", autoplay_policy_name(autoplay.policy)));
        use_default->setCheckable(true);
        use_default->setChecked(!is_allowed_on_site);
        auto* chosen = menu.exec(m_autoplay_button->mapToGlobal(QPoint(0, m_autoplay_button->height())));
        if (chosen == allow)
            WebView::Application::settings().add_autoplay_site_filter(m_host);
        else if (chosen == use_default)
            WebView::Application::settings().remove_autoplay_site_filter(m_host);
        update_autoplay();
    });
    autoplay_row->addWidget(m_autoplay_button);
    layout->addLayout(autoplay_row);

    add_separator(*m_site_page, *layout, 6, 6);

    // Whether content blocking is on for the site, and what it blocked on the page.
    auto* blocking_row = new QHBoxLayout;
    blocking_row->setContentsMargins(10, 0, 10, 0);
    blocking_row->setSpacing(10);
    m_content_blocking_icon = create_icon(*m_site_page);
    blocking_row->addWidget(m_content_blocking_icon);
    auto* blocking_label = new QLabel("Content blocking", m_site_page);
    blocking_label->setFixedHeight(ROW_HEIGHT);
    blocking_row->addWidget(blocking_label, 1);
    m_content_blocking_switch = new PopoverSwitch(m_site_page);
    m_content_blocking_switch->setAccessibleName("Content blocking on this site");
    QObject::connect(m_content_blocking_switch, &QCheckBox::toggled, this, [this](bool enabled) {
        if (on_content_blocking_toggle)
            on_content_blocking_toggle(enabled);
    });
    blocking_row->addWidget(m_content_blocking_switch);
    layout->addLayout(blocking_row);

    m_blocked_row = new QPushButton(m_site_page);
    m_blocked_row->setObjectName("LadybirdPopoverRow");
    m_blocked_row->setAccessibleName("Blocked on this page");
    m_blocked_row->setFixedHeight(ROW_HEIGHT);
    m_blocked_row->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    auto* blocked_row_layout = new QHBoxLayout(m_blocked_row);
    blocked_row_layout->setContentsMargins(10, 0, 10, 0);
    blocked_row_layout->setSpacing(10);
    // NB: A box layout puts no spacing after a spacer, so this spacer covers the icon and its spacing in the rows above.
    blocked_row_layout->addSpacing(26);
    auto* blocked_label = new QLabel("Blocked on this page", m_blocked_row);
    blocked_label->setAttribute(Qt::WA_TransparentForMouseEvents);
    blocked_row_layout->addWidget(blocked_label, 1);
    m_blocked_count = new QLabel(m_blocked_row);
    m_blocked_count->setObjectName("LadybirdSiteSettingsDetail");
    m_blocked_count->setAttribute(Qt::WA_TransparentForMouseEvents);
    blocked_row_layout->addWidget(m_blocked_count);
    m_blocked_row_chevron = new QLabel(m_blocked_row);
    m_blocked_row_chevron->setFixedSize(12, 12);
    m_blocked_row_chevron->setAttribute(Qt::WA_TransparentForMouseEvents);
    blocked_row_layout->addWidget(m_blocked_row_chevron);
    QObject::connect(m_blocked_row, &QPushButton::clicked, this, [this] { show_blocked_resources(true); });
    layout->addWidget(m_blocked_row);

    add_separator(*m_site_page, *layout, 6, 2);

    auto* links = new QHBoxLayout;
    links->setContentsMargins(4, 0, 4, 0);
    // What can be set for the site is reset to the defaults.
    m_reset_permissions_link = create_link(*m_site_page, "Reset permissions");
    QObject::connect(m_reset_permissions_link, &QPushButton::clicked, this, [this] {
        WebView::Application::settings().remove_autoplay_site_filter(m_host);
        update_autoplay();
        if (on_reset_permissions)
            on_reset_permissions();
    });
    links->addWidget(m_reset_permissions_link);
    links->addStretch(1);
    auto* site_settings_link = create_link(*m_site_page, "All site settings");
    QObject::connect(site_settings_link, &QPushButton::clicked, this, [this] {
        close();
        if (on_open_all_site_settings)
            on_open_all_site_settings();
    });
    links->addWidget(site_settings_link);
    layout->addLayout(links);
    pages->addWidget(m_site_page);

    // What was blocked on the page.
    m_blocked_page = new QWidget(&card());
    auto* blocked_layout = new QVBoxLayout(m_blocked_page);
    blocked_layout->setContentsMargins(6, 8, 6, 8);
    blocked_layout->setSpacing(0);

    auto* blocked_heading = new QHBoxLayout;
    blocked_heading->setContentsMargins(4, 0, 10, 0);
    blocked_heading->setSpacing(4);
    m_back_button = new QToolButton(m_blocked_page);
    m_back_button->setObjectName("LadybirdPopoverIconButton");
    m_back_button->setAccessibleName("Back to site settings");
    m_back_button->setFixedSize(28, 28);
    m_back_button->setIconSize({ 14, 14 });
    QObject::connect(m_back_button, &QToolButton::clicked, this, [this] { show_blocked_resources(false); });
    blocked_heading->addWidget(m_back_button);
    auto* blocked_title = new QLabel("Blocked on this page", m_blocked_page);
    blocked_title->setObjectName("LadybirdSiteSettingsHost");
    blocked_title->setFixedHeight(32);
    blocked_heading->addWidget(blocked_title, 1);
    m_blocked_page_count = new QLabel(m_blocked_page);
    m_blocked_page_count->setObjectName("LadybirdSiteSettingsDetail");
    blocked_heading->addWidget(m_blocked_page_count);
    blocked_layout->addLayout(blocked_heading);
    blocked_layout->addSpacing(6);

    add_separator(*m_blocked_page, *blocked_layout, 0, 6);

    m_blocked_list = new QVBoxLayout;
    m_blocked_list->setContentsMargins(10, 0, 10, 0);
    m_blocked_list->setSpacing(0);
    blocked_layout->addLayout(m_blocked_list);

    add_separator(*m_blocked_page, *blocked_layout, 8, 6);

    auto* blocked_links = new QHBoxLayout;
    blocked_links->setContentsMargins(4, 2, 4, 0);
    auto* report_link = create_link(*m_blocked_page, "Report a broken site");
    QObject::connect(report_link, &QPushButton::clicked, this, [this] {
        close();
        if (on_report_broken_site)
            on_report_broken_site();
    });
    blocked_links->addWidget(report_link);
    blocked_links->addStretch(1);
    auto* filter_lists_link = create_link(*m_blocked_page, "Filter lists");
    QObject::connect(filter_lists_link, &QPushButton::clicked, this, [this] {
        close();
        if (on_open_filter_lists)
            on_open_filter_lists();
    });
    blocked_links->addWidget(filter_lists_link);
    blocked_layout->addLayout(blocked_links);
    pages->addWidget(m_blocked_page);
    m_blocked_page->hide();

    update_icons();
}

void SiteSettingsPopover::set_site(Site site)
{
    m_host = AK::move(site.host);
    m_host_label->setText(qstring_from_ak_string(m_host));
    // A secure connection is the norm, so only an insecure one is mentioned.
    m_security_label->setText("Connection is not secure");
    m_security_label->setVisible(!site.is_secure);

    m_favicon->setPixmap(site.favicon.isNull() ? create_chrome_icon(ChromeIcon::Globe, Theme::panel().muted).pixmap(QSize(16, 16), devicePixelRatioF()) : site.favicon.pixmap(QSize(16, 16), devicePixelRatioF()));

    set_permissions(site.permissions);
    update_autoplay();
    show_blocked_resources(false);
    set_content_blocking(site.content_blocking);
}

// A permission the user never decided and the page never asked for has no row.
void SiteSettingsPopover::set_permissions(Vector<WebView::SitePermissionRow> const& permissions)
{
    // The rows go once the event loop is back: a change made from a row's menu arrives from a button in that row.
    auto* rows_layout = m_permission_rows->layout();
    while (auto* item = rows_layout->takeAt(0)) {
        if (auto* widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_permissions.clear();
    m_has_decided_permissions = false;

    for (auto const& permission : permissions) {
        if (!permission.decision.has_value() && !permission.requested_by_page)
            continue;
        m_has_decided_permissions |= permission.decision.has_value();

        auto* row = new QWidget(m_permission_rows);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(10, 0, 10, 0);
        row_layout->setSpacing(10);
        auto* icon = create_icon(*row);
        row_layout->addWidget(icon);
        auto* label = new QLabel(permission_label(permission.name), row);
        label->setFixedHeight(ROW_HEIGHT);
        row_layout->addWidget(label, 1);
        auto* button = create_value_button(*row);
        button->setObjectName(QStringLiteral("LadybirdSiteSettingsPermission-%1").arg(qstring_from_ak_string(Web::Permissions::to_string(permission.name))));
        button->setText(permission_decision_name(permission.decision));
        QObject::connect(button, &QPushButton::clicked, this, [this, button, name = permission.name, decision = permission.decision, one_time_grant_is_live = permission.one_time_grant_is_live] {
            QMenu menu(this);
            Vector<QAction*> actions;
            Vector<Optional<WebView::PermissionDecision>> choices;
            auto add_choice = [&](QString const& text, Optional<WebView::PermissionDecision> choice) {
                auto* action = menu.addAction(text);
                action->setCheckable(true);
                action->setChecked(choice == decision);
                actions.append(action);
                choices.append(choice);
            };
            add_choice("Allow", WebView::PermissionDecision::Allow);
            // A one-time grant can be kept while it lasts, but not given from here.
            if (one_time_grant_is_live)
                add_choice("Allow this time", WebView::PermissionDecision::AllowOnce);
            add_choice("Ask", {});
            add_choice("Block", WebView::PermissionDecision::Block);
            auto* chosen = menu.exec(button->mapToGlobal(QPoint(0, button->height())));
            if (auto index = actions.find_first_index(chosen); index.has_value() && on_permission_change)
                on_permission_change(name, choices[*index]);
        });
        row_layout->addWidget(button);
        rows_layout->addWidget(row);
        m_permissions.append({ .name = permission.name, .icon = icon, .button = button });
    }

    m_permission_rows->setVisible(!m_permissions.is_empty());
    update_icons();
    update_reset_link();
    fit_to_page();
}

void SiteSettingsPopover::set_content_blocking(WebView::ContentBlockingSnapshot const& blocking)
{
    QSignalBlocker blocker(m_content_blocking_switch);
    m_content_blocking_switch->setChecked(blocking.enabled());
    m_content_blocking_switch->setEnabled(blocking.can_toggle());

    // What was blocked is only listed while content blocking is on.
    auto count = qformatted("{}", blocking.blocked_resources.size());
    m_blocked_count->setText(count);
    m_blocked_page_count->setText(count);
    m_blocked_row->setVisible(blocking.enabled());
    if (!blocking.enabled())
        show_blocked_resources(false);

    while (auto* item = m_blocked_list->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (blocking.blocked_resources.is_empty()) {
        auto* empty = new QLabel("Nothing has been blocked on this page.", m_blocked_page);
        empty->setObjectName("LadybirdSiteSettingsDetail");
        empty->setFixedHeight(BLOCKED_RESOURCE_ROW_HEIGHT);
        m_blocked_list->addWidget(empty);
    }
    for (auto const& resource : blocking.blocked_resources) {
        auto* row = new QWidget(m_blocked_page);
        row->setFixedHeight(BLOCKED_RESOURCE_ROW_HEIGHT);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(10);
        auto* host = new QLabel(row);
        host->setTextFormat(Qt::PlainText);
        host->setText(host->fontMetrics().elidedText(qstring_from_ak_string(resource.host), Qt::ElideRight, POPOVER_WIDTH - 120));
        row_layout->addWidget(host, 1);
        auto* kind = new QLabel(qstring_from_ak_string(resource.kind), row);
        kind->setObjectName("LadybirdSiteSettingsDetail");
        row_layout->addWidget(kind);
        m_blocked_list->addWidget(row);
    }
    fit_to_page();
}

void SiteSettingsPopover::show_blocked_resources(bool show)
{
    // NB: The page shown before is hidden first, so that the popover never has to fit both pages at once.
    (show ? m_site_page : m_blocked_page)->hide();
    (show ? m_blocked_page : m_site_page)->show();
    fit_to_page();
}

void SiteSettingsPopover::fit_to_page()
{
    // NB: The page's and the card's layouts take in a row that shows or hides only once they are activated, so they are
    //     activated before the popover's own, which would otherwise keep the card at its old height.
    for (auto* page : { m_site_page, m_blocked_page }) {
        if (!page->isHidden())
            page->layout()->activate();
    }
    card().layout()->activate();
    layout()->activate();
    adjustSize();
}

void SiteSettingsPopover::update_autoplay()
{
    auto const& autoplay = WebView::Application::settings().autoplay_settings();
    auto name = autoplay.site_filters.contains(m_host) ? QStringLiteral("Allow") : autoplay_policy_name(autoplay.policy);
    m_autoplay_button->setText(name);
    m_autoplay_button->setIcon(create_chrome_icon(ChromeIcon::ChevronDown, Theme::panel().muted));
    update_reset_link();
}

// There is something to reset once the site has a decision of its own, for a permission or for autoplay.
void SiteSettingsPopover::update_reset_link()
{
    if (!m_reset_permissions_link)
        return;
    auto const& autoplay = WebView::Application::settings().autoplay_settings();
    m_reset_permissions_link->setEnabled(m_has_decided_permissions || autoplay.site_filters.contains(m_host));
}

void SiteSettingsPopover::update_chrome_style(QPalette const& palette)
{
    setPalette(palette);
    setStyleSheet(ChromeStyle::site_settings_popover_style_sheet(palette));
    // NB: The popover is kept for its tab, so its icons are drawn again in the colors of the current theme.
    update_icons();
}

void SiteSettingsPopover::update_icons()
{
    auto const& panel = Theme::panel();
    for (auto& permission : m_permissions) {
        permission.icon->setPixmap(create_chrome_icon(permission_icon(permission.name), panel.body).pixmap(QSize(16, 16), devicePixelRatioF()));
        permission.button->setIcon(create_chrome_icon(ChromeIcon::ChevronDown, panel.muted));
    }
    m_autoplay_icon->setPixmap(create_chrome_icon(ChromeIcon::Volume, panel.body).pixmap(QSize(16, 16), devicePixelRatioF()));
    m_content_blocking_icon->setPixmap(create_chrome_icon(ChromeIcon::ContentBlocking, panel.body).pixmap(QSize(16, 16), devicePixelRatioF()));
    m_blocked_row_chevron->setPixmap(create_chrome_icon(ChromeIcon::Forward, panel.muted).pixmap(QSize(12, 12), devicePixelRatioF()));
    m_back_button->setIcon(create_chrome_icon(ChromeIcon::Back, panel.body));
}

}
