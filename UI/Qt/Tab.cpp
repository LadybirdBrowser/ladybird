/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/kmalloc.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Promise.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWakeLock/DisplaySleepInhibitor.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebView/Application.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BookmarkPopover.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/JavaScriptDialog.h>
#include <UI/Qt/LadybirdButton.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/Menu.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/PermissionPromptPopover.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/RestoreSessionCard.h>
#include <UI/Qt/SiteSettingsPopover.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>

#include <QApplication>
#include <QCalendarWidget>
#include <QClipboard>
#include <QColorDialog>
#include <QCursor>
#include <QDialogButtonBox>
#include <QDynamicPropertyChangeEvent>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImage>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimeEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int HOVER_TEXT_CORNER_RADIUS = 4;

static constexpr int BOOKMARKS_BAR_HOVER_COLLAPSE_POLL_INTERVAL_MS = 250;

// The calendar's month buttons get chevrons in the palette's text color, since the style's own arrows stay dark in dark
// mode.
enum class ChevronDirection {
    Left,
    Right,
};

static QIcon calendar_month_chevron_icon(QWidget const& widget, ChevronDirection direction)
{
    bool const points_left = direction == ChevronDirection::Left;
    static constexpr int size = 16;
    auto device_pixel_ratio = widget.devicePixelRatioF();
    QPixmap pixmap(QSize(size, size) * device_pixel_ratio);
    pixmap.setDevicePixelRatio(device_pixel_ratio);
    pixmap.fill(Qt::transparent);

    QPainterPath chevron;
    chevron.moveTo(points_left ? 10 : 6, 4);
    chevron.lineTo(points_left ? 6 : 10, 8);
    chevron.lineTo(points_left ? 10 : 6, 12);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(widget.palette().color(QPalette::WindowText), 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(chevron);
    return QIcon(pixmap);
}

static constexpr int TAB_FAVICON_SIZE = 16;
static constexpr int PRIVATE_SESSION_POPOVER_WIDTH = 320;

// The page's zoom, with buttons to change it and a way back to the default.
class ZoomPopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ZoomPopover(QWidget* parent)
        : Popover(parent)
    {
        setAccessibleName("Page zoom");
        card().setObjectName("LadybirdZoomPopover");
        card().setFixedWidth(232);

        auto* layout = new QVBoxLayout(&card());
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(10);

        auto* controls = new QHBoxLayout;
        controls->setSpacing(6);
        auto make_button = [&](ChromeIcon icon, QString const& name, auto callback) {
            auto* button = new QPushButton(&card());
            button->setObjectName("LadybirdZoomButton");
            button->setIcon(create_chrome_icon(icon, Theme::panel().body));
            button->setIconSize({ 16, 16 });
            button->setFixedSize(30, 28);
            button->setAccessibleName(name);
            button->setToolTip(name);
            QObject::connect(button, &QPushButton::clicked, this, [this, callback] {
                callback(*this);
                update_level();
            });
            return button;
        };
        controls->addWidget(make_button(ChromeIcon::ZoomOut, "Zoom out", [](ZoomPopover& popover) { if (popover.on_zoom_out) popover.on_zoom_out(); }));
        m_level = new QLabel(&card());
        m_level->setObjectName("LadybirdZoomLevel");
        m_level->setAlignment(Qt::AlignCenter);
        controls->addWidget(m_level, 1);
        controls->addWidget(make_button(ChromeIcon::NewTab, "Zoom in", [](ZoomPopover& popover) { if (popover.on_zoom_in) popover.on_zoom_in(); }));
        layout->addLayout(controls);

        auto* footer = new QHBoxLayout;
        footer->addStretch(1);
        auto* reset = new QPushButton("Reset", &card());
        reset->setObjectName("LadybirdPopoverLink");
        reset->setCursor(Qt::PointingHandCursor);
        QObject::connect(reset, &QPushButton::clicked, this, [this] {
            if (on_reset)
                on_reset();
            update_level();
        });
        footer->addWidget(reset);
        footer->addStretch(1);
        layout->addLayout(footer);
    }

    void update_level()
    {
        if (current_level)
            m_level->setText(qformatted("{}%", round_to<int>(current_level() * 100)));
    }

    Function<void()> on_zoom_in;
    Function<void()> on_zoom_out;
    Function<void()> on_reset;
    Function<double()> current_level;

private:
    QLabel* m_level { nullptr };
};

Tab::Tab(BrowserWindow* window, Optional<WebView::CanonicalTraversable&> traversable)
    : QWidget(window)
    , m_window(window)
{
    create_tab_shell();
    initialize(traversable);
}

Tab::Tab(BrowserWindow* window, DeferredTab deferred_tab)
    : QWidget(window)
    , m_window(window)
    , m_title(deferred_tab.title)
    , m_favicon(favicon_with_rounded_corners(deferred_tab.favicon, TAB_FAVICON_SIZE))
    , m_session_tab_id(deferred_tab.session_tab_id)
    , m_deferred_tab(AK::move(deferred_tab))
{
    create_tab_shell();
}

void Tab::create_tab_shell()
{
    auto* tab_layout = new QBoxLayout(QBoxLayout::Direction::TopToBottom, this);
    tab_layout->setSpacing(0);
    tab_layout->setContentsMargins(0, 0, 0, 0);

    create_tab_context_menu();
}

void Tab::initialize(Optional<WebView::CanonicalTraversable&> traversable)
{
    auto* tab_layout = static_cast<QBoxLayout*>(layout());

    auto view_initial_state = WebContentViewInitialState {
        .is_private = m_window->is_private(),
        .maximum_frames_per_second = m_window->refresh_rate(),
        .display_id = m_window->display_id(),
        .owner_view = nullptr,
    };

    m_view = new WebContentView(this, traversable, AK::move(view_initial_state));
    // The window reports its geometry to its tabs as it is moved and resized, so a tab opened later has to be told
    // where the window stands now.
    m_view->set_window_position({ m_window->x(), m_window->y() });
    m_view->set_window_size({ m_window->width(), m_window->height() });
    m_javascript_dialog = new JavaScriptDialog(m_view);
    m_javascript_dialog->on_page_dim_change = [this](qreal dim) { m_view->set_page_dim(dim); };
    m_find_in_page = new FindInPageWidget(this, m_view);
    m_view->add_floating_surface(*m_find_in_page);
    m_find_in_page->setVisible(false);

    // The window's toolbar holds the address field while it shows this tab.
    m_location_edit = new LocationEdit(this, m_window->is_private());
    m_location_edit->hide();

    m_bookmarks_bar = new BookmarksBar(this);
    m_bookmarks_bar_container = new QWidget(this);
    m_bookmarks_bar_container->setObjectName("LadybirdBookmarksBarContainer");
    m_bookmarks_bar_container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
#if defined(AK_OS_MACOS) || defined(LADYBIRD_QT_USE_RHI_WIDGET)
    // The web view is native on these presentation paths, so its sibling overlay must also be native.
    m_bookmarks_bar_container->setAttribute(Qt::WA_DontCreateNativeAncestors);
    m_bookmarks_bar_container->setAttribute(Qt::WA_NativeWindow);
#endif
    m_bookmarks_bar->setParent(m_bookmarks_bar_container);
    m_bookmarks_bar->show();

    m_bookmarks_bar_hover_expand_timer = new QTimer(this);
    m_bookmarks_bar_hover_expand_timer->setSingleShot(true);
    m_bookmarks_bar_hover_expand_timer->setTimerType(Qt::PreciseTimer);
    connect(m_bookmarks_bar_hover_expand_timer, &QTimer::timeout, this, [this] {
        if (cursor_is_over_bookmarks_bar_hover_region())
            set_bookmarks_bar_hover_expanded(true);
    });

    m_bookmarks_bar_hover_expand_animation = new QVariantAnimation(this);
    m_bookmarks_bar_hover_expand_animation->setEasingCurve(QEasingCurve::OutCubic);
    m_bookmarks_bar_hover_expand_animation->setStartValue(0.0);
    m_bookmarks_bar_hover_expand_animation->setEndValue(1.0);
    connect(m_bookmarks_bar_hover_expand_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_bookmarks_bar_hover_expand_progress = value.toReal();
        update_bookmarks_bar_geometry();
    });

    m_bookmarks_bar_hover_collapse_timer = new QTimer(this);
    m_bookmarks_bar_hover_collapse_timer->setInterval(BOOKMARKS_BAR_HOVER_COLLAPSE_POLL_INTERVAL_MS);
    connect(m_bookmarks_bar_hover_collapse_timer, &QTimer::timeout, this, &Tab::update_bookmarks_bar_hover);

    m_window->toolbar().installEventFilter(this);
    m_bookmarks_bar_container->installEventFilter(this);
    m_bookmarks_bar->installEventFilter(this);
    m_view->installEventFilter(this);
    m_location_edit->installEventFilter(this);

    m_hover_label = new HyperlinkLabel(this);
    m_hover_label->hide();
    m_hover_label->setContentsMargins(4, 2, 4, 2);
    m_hover_label->setAutoFillBackground(true);

    QObject::connect(m_hover_label, &HyperlinkLabel::mouse_entered, [this] {
        update_hover_label();
    });

    auto* focus_location_editor_action = new QAction("Edit Location", this);
    focus_location_editor_action->setShortcuts({ QKeySequence("Ctrl+L"), QKeySequence("Alt+D") });
    addAction(focus_location_editor_action);

    tab_layout->addWidget(m_view);
    update_bookmarks_bar_visibility();

    // NB: A page can block many requests in a burst while it loads, so the open popover catches up with them at most
    //     ten times a second rather than for each one.
    m_content_blocking_update_timer = new QTimer(this);
    m_content_blocking_update_timer->setSingleShot(true);
    m_content_blocking_update_timer->setInterval(100);
    QObject::connect(m_content_blocking_update_timer, &QTimer::timeout, this, [this] {
        if (m_site_settings_popover && m_site_settings_popover->isVisible())
            m_site_settings_popover->set_content_blocking(view().content_blocking_snapshot());
    });
    view().on_content_blocking_change = [this](auto const&) {
        if (m_site_settings_popover && m_site_settings_popover->isVisible() && !m_content_blocking_update_timer->isActive())
            m_content_blocking_update_timer->start();
    };

    // The star bookmarks the page, then shows the bookmark to name it or file it, rather than toggling it.
    auto* bookmark_action = create_application_action(*m_location_edit, view().toggle_bookmark_action());
    QObject::disconnect(bookmark_action, &QAction::triggered, nullptr, nullptr);
    QObject::connect(bookmark_action, &QAction::triggered, this, [this] { show_bookmark_popover(); });
    m_location_edit->set_trailing_action(bookmark_action);
    m_location_edit->set_zoom_action(create_application_action(*m_location_edit, view().reset_zoom_action(), IncludeActionIcon::No));

    view().on_performance_stats = [this](WebView::TabPerformanceStats const& stats) {
        m_performance_stats = stats;
        emit performance_stats_changed();
    };

    recreate_location_edit_icons();
    update_chrome_style();

    m_location_edit->on_site_settings_requested = [this] {
        show_site_settings_popover();
    };
    m_location_edit->on_zoom_requested = [this] {
        show_zoom_popover();
    };

    QObject::connect(m_view, &WebContentView::crash_overlay_changed, this, [this] {
        update_tab_icon();
    });

    view().on_activate_tab = [this] {
        m_window->activate_tab(tab_index());
    };

    view().on_close = [this] {
        auto index = tab_index();
        if (m_tab_to_activate_on_close) {
            if (auto index_to_activate = m_window->tab_index(m_tab_to_activate_on_close); index_to_activate >= 0)
                m_window->activate_tab(index_to_activate);
        }
        if (!m_window_is_closing)
            m_window->definitely_close_tab(index);
    };

    view().on_request_web_compat_report_review = [this](WebCompatReportCapture capture) {
        auto& tab = m_window->new_tab_from_url(URL::about_blank(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::after_tab(*this));
        tab.view().on_close_web_compat_report_tab = [&tab] {
            tab.m_tab_to_activate_on_close = nullptr;
            tab.view().on_close();
        };
        tab.view().review_web_compat_report(AK::move(capture));
        tab.enter_report_review(this, tr("Report Website Issue"));
    };

    // A review takes its screenshot on the page it reports, in the tab it was opened from, and comes back once done.
    view().on_request_web_compat_report_screenshot = [this](Function<void(QImage)> on_selected) {
        auto* reported_tab = m_tab_to_activate_on_close.data();
        if (!reported_tab)
            return;
        m_window->activate_tab(m_window->tab_index(reported_tab));
        reported_tab->view().select_web_compat_report_screenshot(AK::move(on_selected), [this, review_tab = QPointer { this }] {
            if (review_tab)
                m_window->activate_tab(m_window->tab_index(this));
        });
    };

    QObject::connect(m_view, &WebContentView::image_overlay_unavailable, this, &Tab::update_hover_label);

    view().on_link_hover = [this](auto const& url) {
        m_hover_text = qstring_from_ak_string(url.to_byte_string());
        m_hover_text_is_on_right = false;
        update_hover_label();
    };

    view().on_link_unhover = [this]() {
        hide_hover_label();
    };

    view().on_loading_state_change = [this](bool is_loading) {
        set_loading(is_loading);
    };
    view().on_load_start = [this] {
        m_window->on_tab_load_start(*this);
    };
    set_loading(view().is_loading());

    view().on_load_finish = [this](auto const&) {
        m_suppress_javascript_dialogs_until_navigation = false;
    };

    view().on_top_level_navigation_commit = [this] {
        m_is_waiting_for_response = false;
        m_suppress_javascript_dialogs_until_navigation = false;
        m_javascript_dialogs_shown_by_document = 0;
        m_javascript_dialogs_blocked_for_document = false;
    };

    view().on_browser_history_traversal_complete = [this] {
        m_suppress_javascript_dialogs_until_navigation = false;
        m_javascript_dialogs_shown_by_document = 0;
        m_javascript_dialogs_blocked_for_document = false;
    };

    view().on_url_change = [this](auto const& url) {
        m_location_edit->set_url(url);
    };

    m_location_edit->on_navigation = [this](auto input, auto fallback_url, auto destination_kind) {
        location_edit_return_pressed(AK::move(input), AK::move(fallback_url), destination_kind);
    };
    QObject::connect(m_location_edit, &LocationEdit::focus_return_requested, this, [this] {
        view().setFocus();
    });

    view().on_title_change = [this](auto const& title) {
        auto new_title = qstring_from_utf16_string(title);
        if (!m_is_loading && !m_title.isEmpty() && new_title != m_title && m_window->current_tab() != this)
            m_needs_attention = true;
        m_title = AK::move(new_title);
        update_tab_title();
    };

    view().on_favicon_change = [this](auto const& bitmap) {
        if (!bitmap.has_value()) {
            m_favicon = {};
            update_tab_icon();
            return;
        }

        auto qimage = QImage(bitmap->scanline_u8(0), bitmap->width(), bitmap->height(), QImage::Format_ARGB32);
        if (qimage.isNull())
            return;
        auto qpixmap = QPixmap::fromImage(qimage);
        if (qpixmap.isNull())
            return;

        m_favicon = favicon_with_rounded_corners(qpixmap);
        update_tab_icon();
    };

    // A dialog from a page says which site it comes from.
    auto javascript_dialog_title = [this] {
        auto origin = view().url().origin();
        if (!origin.is_opaque())
            return qformatted("{} says", qstring_from_ak_string(origin.host().serialize()));
        return qformatted("{}:// says", qstring_from_ak_string(view().url().scheme()));
    };

    auto offer_to_block_further_dialogs = [this] {
        return m_javascript_dialogs_shown_by_document++ > 0 ? JavaScriptDialog::OfferToBlockFurtherDialogs::Yes : JavaScriptDialog::OfferToBlockFurtherDialogs::No;
    };

    view().on_request_alert = [this, javascript_dialog_title, offer_to_block_further_dialogs](auto const& message) {
        if (m_suppress_javascript_dialogs_until_navigation || m_javascript_dialogs_blocked_for_document) {
            view().alert_closed();
            return;
        }
        hide_hover_label();
        m_javascript_dialog->set_title_icon(tab_icon());
        m_javascript_dialog->show_alert(javascript_dialog_title(), qstring_from_utf16_string(message), offer_to_block_further_dialogs());
    };

    view().on_request_before_unload = [this](auto const& source, auto on_complete) {
        hide_hover_label();
        m_before_unload_completion = AK::move(on_complete);
        m_window->raise_for_close_prompt();
        m_javascript_dialog->set_title_icon(tab_icon());
        m_javascript_dialog->show_before_unload(qstring_from_ak_string(source));
    };

    view().on_request_confirm = [this, javascript_dialog_title, offer_to_block_further_dialogs](auto const& message) {
        if (m_suppress_javascript_dialogs_until_navigation || m_javascript_dialogs_blocked_for_document) {
            view().confirm_closed(false);
            return;
        }
        hide_hover_label();
        m_javascript_dialog->set_title_icon(tab_icon());
        m_javascript_dialog->show_confirm(javascript_dialog_title(), qstring_from_utf16_string(message), offer_to_block_further_dialogs());
    };

    view().on_request_prompt = [this, javascript_dialog_title, offer_to_block_further_dialogs](auto const& message, auto const& default_) {
        if (m_suppress_javascript_dialogs_until_navigation || m_javascript_dialogs_blocked_for_document) {
            view().prompt_closed({});
            return;
        }
        hide_hover_label();
        m_javascript_dialog->set_title_icon(tab_icon());
        m_javascript_dialog->show_prompt(javascript_dialog_title(), qstring_from_utf16_string(message), qstring_from_utf16_string(default_), offer_to_block_further_dialogs());
    };

    view().on_request_set_prompt_text = [this](auto const& message) {
        m_javascript_dialog->set_prompt_text(qstring_from_utf16_string(message));
    };

    m_javascript_dialog->on_complete = [this](auto type, bool accepted, auto const& prompt_text) {
        if (m_javascript_dialog->blocks_further_dialogs())
            m_javascript_dialogs_blocked_for_document = true;
        switch (type) {
        case JavaScriptDialog::Type::Alert:
            view().alert_closed();
            break;
        case JavaScriptDialog::Type::BeforeUnload: {
            auto on_complete = AK::move(m_before_unload_completion);
            VERIFY(on_complete);
            on_complete(accepted);
            if (!accepted) {
                if (!m_preserve_close_retry_state_on_dialog_dismissal)
                    m_already_requested_close = false;
                m_suppress_javascript_dialogs_until_navigation = false;
            }
            break;
        }
        case JavaScriptDialog::Type::Confirm:
            view().confirm_closed(accepted);
            break;
        case JavaScriptDialog::Type::Prompt:
            if (accepted)
                view().prompt_closed(utf16_string_from_qstring(prompt_text));
            else
                view().prompt_closed({});
            break;
        }
    };

    view().on_before_browser_initiated_navigation = [this] {
        m_suppress_javascript_dialogs_until_navigation = true;
        m_javascript_dialog->dismiss();
    };

    view().on_web_content_crashed = [this](auto) {
        m_suppress_javascript_dialogs_until_navigation = false;
        m_javascript_dialog->reset();

        // Close pickers owned by the crashed renderer with their completion signals disconnected; they
        // must not reply to its replacement.
        if (m_color_picker_dialog) {
            auto* dialog = m_color_picker_dialog.data();
            m_color_picker_dialog = nullptr;
            QObject::disconnect(dialog, nullptr, this, nullptr);
            dialog->close();
            dialog->deleteLater();
        }
        if (m_date_picker_dialog) {
            auto* dialog = m_date_picker_dialog.data();
            m_date_picker_dialog = nullptr;
            QObject::disconnect(dialog, nullptr, this, nullptr);
            dialog->close();
            dialog->deleteLater();
        }
        if (m_file_picker_dialog) {
            auto* dialog = m_file_picker_dialog.data();
            m_file_picker_dialog = nullptr;
            QObject::disconnect(dialog, nullptr, this, nullptr);
            dialog->close();
        }
        if (m_external_url_confirmation_dialog)
            m_external_url_confirmation_dialog->close();
        m_permission_prompts.ignore_all();
    };

    view().on_request_external_url_confirmation = [this](auto const& url, auto const& initiator_origin, auto const& handler, auto on_complete) {
        if (m_javascript_dialog->is_open() || m_color_picker_dialog || m_external_url_confirmation_dialog) {
            on_complete(false);
            return;
        }

        hide_hover_label();

        auto initiator = initiator_origin.is_opaque() ? "This page"_string : initiator_origin.serialize();
        auto application_name = handler.application_name().is_empty() ? "another application"sv : handler.application_name().bytes_as_string_view();
        auto title = handler.application_name().is_empty() ? "Open external application?"_string : MUST(String::formatted("Open {}?", application_name));
        auto message = MUST(String::formatted("{} wants to open a {} link with {}.", initiator, url.scheme(), application_name));
        auto details = MUST(String::formatted("URL: '{}'", url));
        auto open_button_text = handler.application_name().is_empty() ? "Open"_string : MUST(String::formatted("Open {}", application_name));

        auto* dialog = new QMessageBox(&view());
        m_external_url_confirmation_dialog = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setIcon(QMessageBox::Icon::Question);
        dialog->setWindowTitle(qstring_from_ak_string(title));
        dialog->setTextFormat(Qt::PlainText);
        dialog->setText(qstring_from_ak_string(message));
        dialog->setDetailedText(qstring_from_ak_string(details));
        auto* open_button = dialog->addButton(qstring_from_ak_string(open_button_text), QMessageBox::ButtonRole::AcceptRole);
        dialog->addButton(QMessageBox::StandardButton::Cancel);
        dialog->setDefaultButton(QMessageBox::StandardButton::Cancel);

        QObject::connect(dialog, &QDialog::finished, this, [this, dialog = QPointer<QMessageBox> { dialog }, open_button, on_complete = AK::move(on_complete)](auto) mutable {
            auto accepted = dialog && dialog->clickedButton() == open_button;
            if (m_external_url_confirmation_dialog == dialog)
                m_external_url_confirmation_dialog = nullptr;
            on_complete(accepted);
        });

        dialog->open();
    };

    // A site's request for a permission hangs from the site settings button, or from the field when the page has no
    // such button.
    m_permission_prompts.create_popover = [this](WebView::PermissionPromptRequest const& request) {
        auto* popover = new PermissionPromptPopover(this, request);
        popover->update_chrome_style(palette());
        auto position = [this, popover] {
            auto& anchor = m_location_edit->site_settings_button();
            QWidget& anchor_or_field = anchor.isVisible() ? anchor : static_cast<QWidget&>(*m_location_edit);
            move_popover_below(*popover, anchor_or_field, PopoverAlignment::Leading);
        };
        popover->adjustSize();
        position();
        popover->on_shown = AK::move(position);
        return popover;
    };

    view().on_request_permission = [this](WebView::PermissionPromptRequest const& request, WebView::PermissionPromptCallback on_outcome) {
        // A tab opened in the background has never been shown, so no hideEvent has suspended its prompts yet.
        if (!isVisible())
            m_permission_prompts.suspend();
        m_permission_prompts.enqueue(request, AK::move(on_outcome));
    };

    view().on_cancel_permission_prompt = [this](WebView::PermissionPromptRequest const& request) {
        m_permission_prompts.cancel(request);
    };

    view().on_location_in_use_change = [this](bool in_use) {
        m_location_edit->set_location_in_use(in_use);
    };

    view().on_request_accept_dialog = [this]() {
        m_javascript_dialog->accept();
    };

    view().on_request_dismiss_dialog = [this]() {
        m_javascript_dialog->dismiss();
    };

    view().on_request_color_picker = [this](Color current_color) {
        m_color_picker_dialog = new QColorDialog(QColor(current_color.red(), current_color.green(), current_color.blue()), &view());

        auto& dialog = *m_color_picker_dialog;
        dialog.setWindowTitle("Ladybird");
        dialog.setOption(QColorDialog::ShowAlphaChannel, false);
        QObject::connect(&dialog, &QColorDialog::currentColorChanged, this, [this](QColor const& color) {
            view().color_picker_update(Color(color.red(), color.green(), color.blue()), Web::HTML::ColorPickerUpdateState::Update);
        });

        QObject::connect(m_color_picker_dialog, &QDialog::finished, this, [this](auto result) {
            if (result == QDialog::Accepted) {
                auto& dialog = *m_color_picker_dialog;
                view().color_picker_update(Color(dialog.selectedColor().red(), dialog.selectedColor().green(), dialog.selectedColor().blue()), Web::HTML::ColorPickerUpdateState::Closed);
            } else {
                view().color_picker_update({}, Web::HTML::ColorPickerUpdateState::Closed);
            }

            m_color_picker_dialog = nullptr;
        });

        m_color_picker_dialog->open();
    };

    view().on_request_date_picker = [this](Utf16String const& input_type, Utf16String const& value, Utf16String const& minimum, Utf16String const& maximum) {
        if (m_date_picker_dialog) {
            view().date_picker_closed({});
            return;
        }

        // NB: The values come from the page, so anything that does not parse leaves the picker at its defaults. The
        //     page sanitizes the value the picker sends back.
        auto type = qstring_from_utf16_string(input_type);
        bool const picks_date = type != "time";
        bool const picks_time = type == "time" || type == "datetime-local";

        auto parse_date = [&](QString const& string) -> QDate {
            if (type == "month")
                return QDate::fromString(string + "-01", "yyyy-MM-dd");
            if (type == "week") {
                auto parts = string.split("-W");
                if (parts.size() != 2)
                    return {};
                bool year_ok = false;
                bool week_ok = false;
                auto year = parts[0].toInt(&year_ok);
                auto week = parts[1].toInt(&week_ok);
                if (!year_ok || !week_ok)
                    return {};
                // The first ISO week of a year holds January 4th.
                auto january_fourth = QDate(year, 1, 4);
                return january_fourth.addDays(1 - january_fourth.dayOfWeek() + (week - 1) * 7);
            }
            return QDate::fromString(string.left(10), "yyyy-MM-dd");
        };
        auto current_value = qstring_from_utf16_string(value);
        auto current_time = type == "datetime-local" ? current_value.mid(11) : current_value;
        // NB: A value with seconds or a fraction of a second keeps them, so that confirming the picker does not drop
        //     them. The "z" format reads and writes one to three digits of the fraction.
        bool const picks_seconds = current_time.length() > 5;
        bool const picks_milliseconds = current_time.contains('.');
        auto const time_format = picks_milliseconds ? QStringLiteral("HH:mm:ss.z")
            : picks_seconds                         ? QStringLiteral("HH:mm:ss")
                                                    : QStringLiteral("HH:mm");
        auto* dialog = new QDialog(&view());
        m_date_picker_dialog = dialog;
        dialog->setWindowTitle("Ladybird");
        auto* layout = new QVBoxLayout(dialog);

        QCalendarWidget* calendar = nullptr;
        if (picks_date) {
            calendar = new QCalendarWidget(dialog);
            calendar->setGridVisible(false);
            calendar->setVerticalHeaderFormat(type == "week" ? QCalendarWidget::ISOWeekNumbers : QCalendarWidget::NoVerticalHeader);
            calendar->setFirstDayOfWeek(type == "week" ? Qt::Monday : QLocale().firstDayOfWeek());
            // Draw weekends like other days, rather than in Qt's default red.
            calendar->setWeekdayTextFormat(Qt::Saturday, {});
            calendar->setWeekdayTextFormat(Qt::Sunday, {});
            calendar->setStyleSheet(
                "#qt_calendar_navigationbar { background: transparent; }"
                "#qt_calendar_navigationbar QToolButton { background: transparent; border: none; padding: 2px 6px; font-weight: 600; }"
                "#qt_calendar_navigationbar QToolButton::menu-indicator { image: none; width: 0; }"
                "#qt_calendar_navigationbar QToolButton:hover { background: palette(midlight); border-radius: 4px; }"
                "QCalendarWidget QAbstractItemView { selection-background-color: palette(accent); selection-color: palette(highlighted-text); }");
            if (auto* previous_month = calendar->findChild<QToolButton*>("qt_calendar_prevmonth"))
                previous_month->setIcon(calendar_month_chevron_icon(*calendar, ChevronDirection::Left));
            if (auto* next_month = calendar->findChild<QToolButton*>("qt_calendar_nextmonth"))
                next_month->setIcon(calendar_month_chevron_icon(*calendar, ChevronDirection::Right));
            if (auto date = parse_date(current_value); date.isValid())
                calendar->setSelectedDate(date);
            if (auto date = parse_date(qstring_from_utf16_string(minimum)); date.isValid())
                calendar->setMinimumDate(date);
            if (auto date = parse_date(qstring_from_utf16_string(maximum)); date.isValid())
                calendar->setMaximumDate(date);
            layout->addWidget(calendar);
        }

        QTimeEdit* time_edit = nullptr;
        if (picks_time) {
            time_edit = new QTimeEdit(dialog);
            time_edit->setDisplayFormat(picks_milliseconds ? QStringLiteral("HH:mm:ss.zzz") : time_format);
            if (auto time = QTime::fromString(current_time, time_format); time.isValid())
                time_edit->setTime(time);
            layout->addWidget(time_edit);
        }

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
        QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        layout->addWidget(buttons);

        QObject::connect(dialog, &QDialog::finished, this, [this, dialog, calendar, time_edit, type, time_format](int result) {
            Optional<Utf16String> picked_value;
            if (result == QDialog::Accepted) {
                QString picked;
                if (calendar) {
                    auto date = calendar->selectedDate();
                    if (type == "month") {
                        picked = date.toString("yyyy-MM");
                    } else if (type == "week") {
                        int week_year = 0;
                        auto week = date.weekNumber(&week_year);
                        picked = QString("%1-W%2").arg(week_year, 4, 10, QChar('0')).arg(week, 2, 10, QChar('0'));
                    } else {
                        picked = date.toString("yyyy-MM-dd");
                    }
                }
                if (time_edit) {
                    if (!picked.isEmpty())
                        picked += "T";
                    picked += time_edit->time().toString(time_format);
                }
                picked_value = utf16_string_from_qstring(picked);
            }
            m_date_picker_dialog = nullptr;
            dialog->deleteLater();
            view().date_picker_closed(AK::move(picked_value));
        });

        dialog->open();
    };

    view().on_request_file_picker = [this](auto const& accepted_file_types, auto allow_multiple_files) {
        QStringList accepted_file_filters;
        QMimeDatabase mime_database;

        for (auto const& filter : accepted_file_types.filters) {
            filter.visit(
                [&](Web::HTML::FileFilter::FileType type) {
                    QString title;
                    QString filter;

                    switch (type) {
                    case Web::HTML::FileFilter::FileType::Audio:
                        title = "Audio files";
                        filter = "audio/";
                        break;
                    case Web::HTML::FileFilter::FileType::Image:
                        title = "Image files";
                        filter = "image/";
                        break;
                    case Web::HTML::FileFilter::FileType::Video:
                        title = "Video files";
                        filter = "video/";
                        break;
                    }

                    QStringList extensions;

                    for (auto const& mime_type : mime_database.allMimeTypes()) {
                        if (mime_type.name().startsWith(filter))
                            extensions.append(mime_type.globPatterns());
                    }

                    accepted_file_filters.append(qformatted("{} ({})", title, extensions.join(" ")));
                },
                [&](Web::HTML::FileFilter::MimeType const& filter) {
                    if (auto mime_type = mime_database.mimeTypeForName(qstring_from_utf16_string(filter.value)); mime_type.isValid())
                        accepted_file_filters.append(mime_type.filterString());
                },
                [&](Web::HTML::FileFilter::Extension const& filter) {
                    accepted_file_filters.append(qformatted("*.{}", qstring_from_utf16_string(filter.value)));
                });
        }

        accepted_file_filters.size() > 1 ? accepted_file_filters.prepend("All files (*)") : accepted_file_filters.append("All files (*)");
        auto filters = accepted_file_filters.join(";;");

        auto allow_multiple = allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes;
        auto* dialog = new QFileDialog(this, allow_multiple ? "Select files" : "Select file", QDir::homePath(), filters);
        m_file_picker_dialog = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setFileMode(allow_multiple ? QFileDialog::ExistingFiles : QFileDialog::ExistingFile);

        QObject::connect(dialog, &QDialog::finished, this, [this, dialog = QPointer<QFileDialog> { dialog }](int result) {
            Vector<Web::HTML::SelectedFile> selected_files;

            if (dialog && result == QDialog::Accepted) {
                for (auto const& path : dialog->selectedFiles()) {
                    auto file_path = ak_byte_string_from_qstring(path);

                    if (auto file = WebView::create_selected_file(file_path); file.is_error())
                        warnln("Unable to open file {}: {}", file_path, file.error());
                    else
                        selected_files.append(file.release_value());
                }
            }

            if (m_file_picker_dialog == dialog)
                m_file_picker_dialog = nullptr;
            view().file_picker_closed(std::move(selected_files));
        });

        dialog->open();
    };

    view().on_find_in_page = [this](auto current_match_index, auto const& total_match_count) {
        m_find_in_page->update_result_label(current_match_index, total_match_count);
    };

    QObject::connect(focus_location_editor_action, &QAction::triggered, this, [this] { focus_location_editor(); });

    view().on_restore_window = [this]() {
        m_window->showNormal();
    };

    view().on_reposition_window = [this](auto const& position) {
        m_window->move(position.x(), position.y());
    };

    view().on_resize_window = [this](auto const& size) {
        m_window->resize(size.width(), size.height());
    };

    view().on_maximize_window = [this]() {
        m_window->showMaximized();
    };

    view().on_minimize_window = [this]() {
        m_window->showMinimized();
    };

    view().on_fullscreen_window = [this]() {
        m_window->fullscreen_mode().enter(this);
    };

    view().on_exit_fullscreen_window = [this]() {
        m_window->fullscreen_mode().exit(FullscreenMode::ExitInitiatedBy::WebContent);
    };

    view().on_audibility_changed = [this](auto audibility) {
        emit audibility_changed(tab_index(), audibility);
    };

    view().on_screen_wake_lock_state_changed = [this](auto wake_lock_state) {
        set_screen_wake_lock_state(wake_lock_state);
    };
}

void Tab::create_tab_context_menu()
{
    auto& application = WebView::Application::the();

    auto* duplicate_tab_action = new QAction("&Duplicate Tab", this);
    QObject::connect(duplicate_tab_action, &QAction::triggered, this, [this]() {
        m_window->duplicate_tab(*this);
    });

    m_pin_tab_action = new QAction("&Pin Tab", this);
    QObject::connect(m_pin_tab_action, &QAction::triggered, this, [this]() {
        if (m_is_pinned)
            m_window->unpin_tab(*this);
        else
            m_window->pin_tab(*this);
    });

    auto* new_tab_to_the_right_action = new QAction("New Tab to the &Right", this);
    QObject::connect(new_tab_to_the_right_action, &QAction::triggered, this, [this]() {
        m_window->new_tab_from_url(WebView::Application::settings().new_tab_page_url(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::after_tab(*this));
    });

    auto* mute_tab_action = new QAction("&Mute Tab", this);
    QObject::connect(mute_tab_action, &QAction::triggered, this, [this]() {
        if (is_loaded()) {
            view().toggle_page_mute_state();
            m_window->update_tab_audio_button(*this);
        }
    });

    auto* move_to_new_window_action = new QAction("Move to New &Window", this);
    QObject::connect(move_to_new_window_action, &QAction::triggered, this, [this]() {
        m_window->detach_tab_to_new_window(tab_index(), m_window->mapToGlobal(QPoint(m_window->width() / 4, m_window->height() / 4)));
    });

    auto* copy_link_action = new QAction("Copy &Link", this);
    QObject::connect(copy_link_action, &QAction::triggered, this, [this]() {
        QGuiApplication::clipboard()->setText(qstring_from_ak_string(url().serialize()));
    });

    auto* close_tab_action = new QAction("&Close Tab", this);
    // NB: The shortcuts belong to the application's actions. The menu only shows them.
    close_tab_action->setShortcut(QKeySequence(QKeySequence::Close));
    close_tab_action->setShortcutContext(Qt::WidgetShortcut);
    close_tab_action->setShortcutVisibleInContextMenu(true);
    QObject::connect(close_tab_action, &QAction::triggered, this, [this]() {
        request_close();
    });

    auto* close_tabs_to_right_action = new QAction("Close Tabs to the R&ight", this);
    QObject::connect(close_tabs_to_right_action, &QAction::triggered, this, [this]() {
        // Closing the current tab selects a neighbor, which may be a tab about to close too, and selecting a tab that
        // hasn't loaded loads it. So, this tab, which stays open, becomes the current tab first.
        if (auto* current = m_window->current_tab(); current && m_window->tab_index(current) > tab_index() && !current->is_pinned())
            m_window->activate_tab(tab_index());
        for (auto i = m_window->tab_count() - 1; i > tab_index(); i--) {
            if (!m_window->tab_at(i)->is_pinned())
                m_window->request_to_close_tab(i);
        }
    });

    auto* close_other_tabs_action = new QAction("Cl&ose Other Tabs", this);
    QObject::connect(close_other_tabs_action, &QAction::triggered, this, [this]() {
        // As in Close Tabs to Right, this tab becomes the current tab first if the current tab is among those closing.
        if (auto* current = m_window->current_tab(); current && current != this && !current->is_pinned())
            m_window->activate_tab(tab_index());
        for (auto i = m_window->tab_count() - 1; i >= 0; i--) {
            if (i == tab_index() || m_window->tab_at(i)->is_pinned())
                continue;

            m_window->request_to_close_tab(i);
        }
    });

    m_context_menu = new QMenu("Context menu", this);
    m_context_menu->addAction(new_tab_to_the_right_action);
    m_context_menu->addSeparator();
    m_context_menu->addAction(create_application_action(*this, application.reload_action(), IncludeActionIcon::No));
    m_context_menu->addAction(duplicate_tab_action);
    m_context_menu->addAction(m_pin_tab_action);
    m_context_menu->addAction(mute_tab_action);
    m_context_menu->addSeparator();
    m_context_menu->addAction(move_to_new_window_action);
    m_context_menu->addAction(copy_link_action);
    m_context_menu->addSeparator();
    m_context_menu->addAction(close_tab_action);
    m_context_menu->addAction(close_other_tabs_action);
    m_context_menu->addAction(close_tabs_to_right_action);
    m_context_menu->addSeparator();
    m_context_menu->addAction(Application::the().reopen_recently_closed_tab_action());

    QObject::connect(m_context_menu, &QMenu::aboutToShow, this, [this, mute_tab_action, move_to_new_window_action]() {
        // NB: A tab that hasn't loaded has no page to mute yet.
        mute_tab_action->setEnabled(is_loaded());
        mute_tab_action->setText(is_loaded() && view().page_mute_state() == Web::HTML::MuteState::Muted ? "Un&mute Tab" : "&Mute Tab");
        move_to_new_window_action->setEnabled(m_window->tab_count() > 1);
    });
}

// The window's toolbar can hold the address field while the tab goes away with the window.
Tab::~Tab()
{
    delete m_location_edit;
}

void Tab::park_location_editing()
{
    // A tab that hasn't loaded has no omnibox yet, so it has no editing session to park.
    if (!is_loaded())
        return;
    m_location_edit->park_editing_session();
}

void Tab::ensure_loaded()
{
    if (is_loaded())
        return;

    // Until initialize() has built the view, url() reads the deferred tab's URL, so the deferred tab stays until then.
    auto deferred_url = m_deferred_tab->url;
    initialize({});
    m_deferred_tab.clear();

    if (m_session_tab_id.has_value())
        view().set_session_tab_id(*m_session_tab_id);
    emit loaded();

    Optional<WebView::SessionHistorySnapshot> history;
    if (m_session_tab_id.has_value())
        history = m_window->session().session_store->load_tab_history(*m_session_tab_id);
    if (!history.has_value() || view().restore_session_history_from_snapshot(history.release_value()).is_error())
        navigate(deferred_url);
}

URL::URL Tab::url() const
{
    if (!is_loaded())
        return m_deferred_tab->url;
    return view().url();
}

Optional<WebView::SessionTabId> Tab::session_tab_id() const
{
    // A loaded tab's view holds the ID it reports its state under.
    if (is_loaded())
        return view().session_tab_id();
    return m_session_tab_id;
}

void Tab::set_session_tab_id(WebView::SessionTabId session_tab_id)
{
    m_session_tab_id = session_tab_id;
    if (is_loaded())
        view().set_session_tab_id(session_tab_id);
}

void Tab::clear_session_tab_id()
{
    m_session_tab_id.clear();
    if (is_loaded())
        view().clear_session_tab_id();
}

void Tab::focus_location_editor(FocusRingTransition focus_ring_transition)
{
    if (!is_loaded())
        return;

#if defined(AK_OS_MACOS)
    make_appkit_window_first_responder(*m_location_edit);
#endif
    m_location_edit->setFocus();
    if (focus_ring_transition == FocusRingTransition::Immediate)
        m_location_edit->show_focus_ring_at_once();
    m_location_edit->selectAll();
    m_location_edit->show_autocomplete();
}

void Tab::set_window(BrowserWindow& window)
{
    if (&window == m_window)
        return;

    if (!is_loaded()) {
        m_window = &window;
        return;
    }

    // The bookmarks bar expands as the pointer comes over the toolbar of the window the tab is in.
    m_window->toolbar().removeEventFilter(this);
    m_window = &window;
    m_window->toolbar().installEventFilter(this);
    recreate_location_edit_icons();

    // The tab's pages hold the geometry of the window that hosted them until now.
    m_view->set_window_position({ window.x(), window.y() });
    m_view->set_window_size({ window.width(), window.height() });
}

void Tab::set_pinned(bool pinned)
{
    m_is_pinned = pinned;
    m_pin_tab_action->setText(pinned ? "Un&pin Tab" : "&Pin Tab");
}

void Tab::navigate(URL::URL const& url)
{
    view().load_from_user_input(url);
}

void Tab::load_html(StringView html)
{
    view().load_html(html);
}

void Tab::location_edit_return_pressed(String input, Optional<URL::URL> fallback_url, WebView::OmniboxDestinationKind destination_kind)
{
    if (input.is_empty())
        return;

    view().set_next_history_visit_transition(WebView::HistoryVisitTransition::Omnibox);
    if (destination_kind == WebView::OmniboxDestinationKind::Search) {
        if (fallback_url.has_value())
            view().load(*fallback_url);
        else
            view().load_navigation_error_page(input);
    } else {
        view().load_from_user_input(input, AK::move(fallback_url));
    }

    auto is_external_url = destination_kind == WebView::OmniboxDestinationKind::URL
        && WebView::classify_user_input(input).classification == WebView::UserInputClassification::ExternalURL;
    if (is_external_url)
        m_location_edit->set_url(view().displayed_url());

    view().setFocus();
}

// A tab whose page crashed shows a warning in place of its favicon.
QIcon Tab::tab_icon() const
{
    if (is_loaded() && view().crash_overlay_active())
        return create_chrome_icon(ChromeIcon::Warning, Theme::chrome(*this).warning);
    return m_favicon;
}

QString Tab::title() const
{
    auto title = m_fixed_title.value_or(m_title);
    // A blank page is what a new tab shows, so it is called a new tab.
    if (!m_fixed_title.has_value() && is_loaded() && view().url() == URL::about_blank() && (title.isEmpty() || title == QStringLiteral("about:blank")))
        title = "New Tab";
    if (!is_loaded() || !WebView::Application::settings().config_variable_as_bool(WebView::ConfigVariableID::ShowWebContentProcessIDInTabTitle))
        return title;

    return QString("%1 [%2]").arg(title).arg(view().web_content_pid());
}

// A tab opened only for a review shows it over a blank page, under the review's own title if it has one, and closing it
// goes back to the tab it was opened from. That lasts until the user takes the tab somewhere else. The tab starts out
// loading about:blank, which is not somewhere else, but any page loaded after that is, about:blank included.
void Tab::enter_report_review(Tab* tab_to_activate_on_close, Optional<QString> title)
{
    VERIFY(!m_report_review_navigation_listener.has_value());
    m_tab_to_activate_on_close = tab_to_activate_on_close;
    m_fixed_title = AK::move(title);
    update_tab_title();

    WebView::ViewImplementation::NavigationListener listener;
    listener.on_load_start = [this](URL::URL const& url) {
        if (m_has_loaded_report_review_page || url != URL::about_blank())
            leave_report_review();
    };
    listener.on_load_finish = [this](URL::URL const&) { m_has_loaded_report_review_page = true; };
    m_report_review_navigation_listener = view().add_navigation_listener(AK::move(listener));
}

// A report on its way keeps being sent, as it does when the tab closes.
void Tab::leave_report_review()
{
    if (!m_report_review_navigation_listener.has_value())
        return;

    view().leave_report_review();
    m_tab_to_activate_on_close = nullptr;
    m_fixed_title.clear();
    update_tab_title();

    // The view is still telling its listeners about this navigation, so the listener goes once it is done.
    QTimer::singleShot(0, this, [this, listener = m_report_review_navigation_listener.release_value()] {
        view().remove_navigation_listener(listener);
    });
}

void Tab::update_tab_title()
{
    emit title_changed(tab_index(), title());
}

void Tab::tab_settings_changed()
{
    if (!is_loaded())
        return;

    recreate_location_edit_icons();
}

void Tab::config_variable_changed(WebView::ConfigVariableID variable)
{
    if (!is_loaded())
        return;

    if (variable == WebView::ConfigVariableID::ShowWebContentProcessIDInTabTitle)
        update_tab_title();
}

qreal Tab::loading_ring_opacity() const
{
    if (m_is_loading)
        return 1;
    if (!m_load_finished_clock.isValid())
        return 0;
    auto elapsed = static_cast<qreal>(m_load_finished_clock.elapsed());
    return max(0.0, 1 - (elapsed / Motion::FAST));
}

qreal Tab::loading_favicon_shrink() const
{
    if (!m_is_loading)
        return loading_ring_opacity();
    auto elapsed = static_cast<qreal>(m_load_started_clock.elapsed());
    return Motion::enter_easing().valueForProgress(min(1.0, elapsed / Motion::BASE));
}

void Tab::set_loading(bool is_loading)
{
    if (m_is_loading == is_loading)
        return;

    m_is_loading = is_loading;
    m_is_waiting_for_response = is_loading;
    if (is_loading)
        m_load_started_clock.start();
    else
        m_load_finished_clock.start();
    emit loading_changed();
}

void Tab::update_tab_icon()
{
    auto index = tab_index();
    if (index < 0)
        return;
    emit favicon_changed(index, tab_icon());
}

void Tab::open_file()
{
    auto filename = QFileDialog::getOpenFileUrl(this, "Open file", QDir::homePath(), "All Files (*.*)");
    if (filename.isValid()) {
        navigate(ak_url_from_qurl(filename));
    }
}

int Tab::tab_index()
{
    return m_window->tab_index(this);
}

void Tab::set_screen_wake_lock_state(Web::ScreenWakeLockState wake_lock_state)
{
    switch (wake_lock_state) {
    case Web::ScreenWakeLockState::Released:
        m_screen_display_sleep_inhibitor.clear();
        break;
    case Web::ScreenWakeLockState::Acquired:
        if (m_screen_display_sleep_inhibitor.has_value())
            break;
        if (auto inhibitor = WakeLock::DisplaySleepInhibitor::create("Ladybird Content"sv); !inhibitor.is_error())
            m_screen_display_sleep_inhibitor = inhibitor.release_value();
        break;
    }
}

void Tab::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (isVisible())
        did_get_real_size();
    if (!is_loaded())
        return;

    if (!m_hover_text.isEmpty())
        update_hover_label();
}

void Tab::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (!is_loaded())
        return;

    set_bookmarks_bar_hover_expanded(false);
    hide_hover_label();

    // A tab's permission prompt waits, unanswered, while another tab is shown.
    m_permission_prompts.suspend();
}

void Tab::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    m_permission_prompts.resume();
}

void Tab::update_hover_label()
{
    if (m_hover_text.isEmpty())
        return;
    auto text = QFontMetrics(m_hover_label->font()).elidedText(m_hover_text, Qt::ElideRight, width() / 2 - 10);

    // NB: Where the view can stack the text above the page itself, it stays inside the window and costs the page
    //     nothing. A tooltip is a window of its own instead, which can stick out of the browser window.
    auto image = render_hover_text(text);
    auto size = image.deviceIndependentSize().toSize();
    QPoint position { m_hover_text_is_on_right ? m_view->width() - size.width() : 0, m_view->height() - size.height() };
    if (m_view->show_image_over_page(image, position)) {
        m_hover_text_rect_over_page = { position, size };
        m_hover_label->hide();
        return;
    }
    if (!m_hover_text_rect_over_page.isEmpty()) {
        m_view->hide_image_over_page();
        m_hover_text_rect_over_page = {};
    }

    m_hover_label->setText(text);
    m_hover_label->setCursor(view().cursor());
    m_hover_label->resize(m_hover_label->sizeHint());

    auto hover_label_height = height() - m_hover_label->height();

    auto left_position = mapToGlobal(QPoint { 0, hover_label_height });

    if (m_hover_label->underMouse() && m_hover_label->pos() == left_position)
        m_hover_label->move(mapToGlobal(QPoint { width() - m_hover_label->width(), hover_label_height }));
    else
        m_hover_label->move(left_position);

    m_hover_label->raise();
    m_hover_label->show();
}

void Tab::hide_hover_label()
{
    m_hover_text = {};
    if (!m_hover_text_rect_over_page.isEmpty()) {
        m_view->hide_image_over_page();
        m_hover_text_rect_over_page = {};
    }
    m_hover_label->hide();
}

// Paints the hover text the way the hover label's style sheet does. It sits in a bottom corner of the page, and only
// its corner toward the page is rounded.
QImage Tab::render_hover_text(QString const& text) const
{
    static constexpr int horizontal_padding = 4 + 1;
    static constexpr int vertical_padding = 2 + 1;

    QFontMetrics metrics { m_hover_label->font() };
    QSize size { metrics.horizontalAdvance(text) + 2 * horizontal_padding, metrics.height() + 2 * vertical_padding };
    auto device_pixel_ratio = devicePixelRatioF();
    QImage image { size * device_pixel_ratio, QImage::Format_ARGB32_Premultiplied };
    image.setDevicePixelRatio(device_pixel_ratio);
    image.fill(Qt::transparent);

    auto palette = Theme::chrome_palette(*this);
    QPainter painter { &image };
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(ChromeStyle::chrome_border(palette));
    painter.setBrush(ChromeStyle::chrome_surface(palette));
    auto rect = QRectF { QPointF {}, size }.adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath path;
    path.moveTo(rect.bottomLeft());
    if (m_hover_text_is_on_right) {
        path.lineTo(rect.left(), rect.top() + HOVER_TEXT_CORNER_RADIUS);
        path.arcTo(rect.left(), rect.top(), 2 * HOVER_TEXT_CORNER_RADIUS, 2 * HOVER_TEXT_CORNER_RADIUS, 180, -90);
        path.lineTo(rect.topRight());
    } else {
        path.lineTo(rect.topLeft());
        path.lineTo(rect.right() - HOVER_TEXT_CORNER_RADIUS, rect.top());
        path.arcTo(rect.right() - 2 * HOVER_TEXT_CORNER_RADIUS, rect.top(), 2 * HOVER_TEXT_CORNER_RADIUS, 2 * HOVER_TEXT_CORNER_RADIUS, 90, -90);
    }
    path.lineTo(rect.bottomRight());
    path.closeSubpath();
    painter.drawPath(path);
    painter.setFont(m_hover_label->font());
    painter.setPen(ChromeStyle::chrome_text(palette));
    painter.drawText(QRect { QPoint {}, size }, Qt::AlignCenter, text);
    return image;
}

bool Tab::event(QEvent* event)
{
    // An inactive window's toolbar icons drop to the muted color.
    if ((event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate) && is_loaded())
        recreate_location_edit_icons();

    if (event->type() == QEvent::PaletteChange && is_loaded()) {
        update_chrome_style();
        recreate_location_edit_icons();
        if (m_is_loading)
            update_tab_icon();
        return QWidget::event(event);
    }

    return QWidget::event(event);
}

bool Tab::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_location_edit && event->type() == QEvent::DynamicPropertyChange) {
        if (static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "autocompleteOpen")
            update_bookmarks_bar_hover();
    }
    // NB: The hover text over the page lets the pointer through, so it moves aside when the pointer comes over it.
    if (watched == m_view && event->type() == QEvent::MouseMove && m_hover_text_rect_over_page.contains(static_cast<QMouseEvent*>(event)->position().toPoint())) {
        m_hover_text_is_on_right = !m_hover_text_is_on_right;
        update_hover_label();
    }
    if (watched == m_view && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
        update_bookmarks_bar_geometry();
        if (m_find_in_page->isVisible())
            m_find_in_page->reposition();
        if (m_restore_session_card)
            m_restore_session_card->reposition();
    }
    if (watched == m_bookmarks_bar_container && event->type() == QEvent::Resize)
        update_bookmarks_bar_geometry();
    if (watched == m_bookmarks_bar && event->type() == QEvent::LayoutRequest)
        update_bookmarks_bar_geometry();

    if (watched == &m_window->toolbar() || watched == m_bookmarks_bar_container) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Show) {
            update_bookmarks_bar_hover();
        } else if (event->type() == QEvent::Leave) {
            QTimer::singleShot(0, this, &Tab::update_bookmarks_bar_hover);
        } else if (event->type() == QEvent::Hide && watched == &m_window->toolbar()) {
            set_bookmarks_bar_hover_expanded(false);
        }
    }

    return QWidget::eventFilter(watched, event);
}

void Tab::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    m_bookmarks_bar->update_chrome_style();
    auto hover_surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface(Theme::chrome_palette(*this)));
    auto hover_border = ChromeStyle::style_sheet_color(ChromeStyle::chrome_border(Theme::chrome_palette(*this)));
    auto hover_text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_text(Theme::chrome_palette(*this)));
    m_hover_label->setStyleSheet(qformatted("background: {}; color: {}; border: 1px solid {}; border-top-right-radius: {}px;",
        hover_surface, hover_text, hover_border, HOVER_TEXT_CORNER_RADIUS));
    m_is_updating_chrome_style = false;
}

void Tab::recreate_location_edit_icons()
{
    if (auto* action = m_location_edit->trailing_action()) {
        auto engaged = view().toggle_bookmark_action().engaged();
        action->setIcon(create_chrome_icon(engaged ? ChromeIcon::StarFilled : ChromeIcon::Star, Theme::chrome_palette(*this), ChromeStyle::window_activity(*this)));
    }
}

void Tab::show_zoom_popover()
{
    if (!m_zoom_popover) {
        auto* popover = new ZoomPopover(this);
        popover->on_zoom_in = [this] { view().zoom_in(); };
        popover->on_zoom_out = [this] { view().zoom_out(); };
        popover->on_reset = [this] {
            view().reset_zoom();
            m_zoom_popover->close();
        };
        popover->current_level = [this] { return view().zoom_level(); };
        m_zoom_popover = popover;
    }

    auto& popover = static_cast<ZoomPopover&>(*m_zoom_popover);
    popover.setStyleSheet(ChromeStyle::zoom_popover_style_sheet(Theme::chrome_palette(*this)));
    popover.update_level();
    popover.adjustSize();
    // The panel hangs from the address field, lined up with its right edge.
    move_popover_below(popover, *m_location_edit);
    popover.show();
    move_popover_below(popover, *m_location_edit);
}

void Tab::show_bookmark_popover()
{
    auto& store = WebView::Application::bookmark_store();
    auto just_added = BookmarkPopover::JustAdded::No;
    auto bookmark = store.find_bookmark_by_url(view().url());
    if (!bookmark.has_value()) {
        Optional<String> title;
        if (auto page_title = view().title().to_utf8(); !page_title.is_empty())
            title = AK::move(page_title);
        m_location_edit->animate_trailing_action_change(LocationEdit::TrailingActionMotion::Pop, [&] {
            store.add_bookmark(view().url(), AK::move(title), view().favicon_hash());
        });
        bookmark = store.find_bookmark_by_url(view().url());
        just_added = BookmarkPopover::JustAdded::Yes;
    }
    if (!bookmark.has_value())
        return;

    if (!m_bookmark_popover) {
        m_bookmark_popover = new BookmarkPopover(this);
        m_bookmark_popover->on_remove = [this](Function<void()> const& remove) {
            m_location_edit->animate_trailing_action_change(LocationEdit::TrailingActionMotion::Fade, remove);
        };
    }
    m_bookmark_popover->update_chrome_style(palette());
    m_bookmark_popover->set_bookmark(bookmark->id, just_added);
    auto& anchor = m_location_edit->trailing_action_button();
    move_popover_below(*m_bookmark_popover, anchor);
    m_bookmark_popover->show();
    move_popover_below(*m_bookmark_popover, anchor);
}

void Tab::close_popovers()
{
    for (auto* popover : { static_cast<QWidget*>(m_private_session_popover), static_cast<QWidget*>(m_site_settings_popover), static_cast<QWidget*>(m_zoom_popover), static_cast<QWidget*>(m_bookmark_popover) }) {
        if (popover)
            popover->close();
    }
}

void Tab::show_site_settings_popover()
{
    auto url = view().url();
    // A page on an inherited about:blank has its opener's origin and no host of its own; the panel is for it too.
    auto origin = view().permission_origin();
    if (!url.host().has_value() && origin.is_opaque())
        return;

    if (!m_site_settings_popover) {
        m_site_settings_popover = new SiteSettingsPopover(this);
        m_location_edit->follow_site_settings_popover(*m_site_settings_popover);
        m_site_settings_popover->on_content_blocking_toggle = [this](bool enabled) {
            auto snapshot = view().content_blocking_snapshot();
            if (!snapshot.site.has_value() || !snapshot.can_toggle())
                return;
            if (WebView::Application::settings().set_content_blocker_enabled_for_site(*snapshot.site, enabled).is_error())
                return;
            view().reload();
        };
        m_site_settings_popover->on_open_all_site_settings = [this] {
            m_window->new_tab_from_url(URL::Parser::basic_parse("about:settings#permissions"sv).release_value(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::after_current_tab());
        };
        m_site_settings_popover->on_report_broken_site = [] {
            WebView::Application::the().report_website_issue_action().activate();
        };
        m_site_settings_popover->on_open_filter_lists = [this] {
            m_window->new_tab_from_url(URL::Parser::basic_parse("about:settings#blocking"sv).release_value(), Web::HTML::ActivateTab::Yes, BrowserWindow::TabLocation::after_current_tab());
        };
        // A change reaches every page of the site at once, and the panel shows it right away. It applies to the site
        // the panel was opened for: its menu runs a nested event loop, during which the tab may move on to another.
        m_site_settings_popover->on_permission_change = [this](Web::Permissions::PermissionName name, Optional<WebView::PermissionDecision> decision) {
            view().set_site_permission(*m_site_settings_popover_origin, name, decision);
            refresh_site_settings_popover();
        };
        m_site_settings_popover->on_reset_permissions = [this] {
            view().reset_site_permissions(*m_site_settings_popover_origin);
            refresh_site_settings_popover();
        };
    }

    m_site_settings_popover_origin = origin;
    m_site_settings_popover->update_chrome_style(palette());
    m_site_settings_popover->set_site({
        .host = url.host().has_value() ? MUST(String::from_utf8(url.serialized_host())) : origin.host().serialize(),
        .is_secure = (url.host().has_value() ? url.scheme() : origin.scheme().value_or({})) == "https"sv,
        .favicon = m_favicon,
        .content_blocking = view().content_blocking_snapshot(),
        .permissions = view().site_permission_rows(),
    });
    // The site settings button is hidden for a page with no host, so the popover hangs from the field then.
    auto& anchor = m_location_edit->site_settings_button();
    QWidget& anchor_or_field = anchor.isVisible() ? anchor : static_cast<QWidget&>(*m_location_edit);
    move_popover_below(*m_site_settings_popover, anchor_or_field, PopoverAlignment::Leading);
    m_site_settings_popover->show();
    move_popover_below(*m_site_settings_popover, anchor_or_field, PopoverAlignment::Leading);
    m_site_settings_popover->raise();
}

// The rows are read again for the site the panel was opened for; a panel whose tab has moved on to another site closes
// instead, since its rows would be the new site's.
void Tab::refresh_site_settings_popover()
{
    if (!m_site_settings_popover)
        return;
    if (!view().permission_origin().is_same_origin(*m_site_settings_popover_origin)) {
        m_site_settings_popover->close();
        return;
    }
    m_site_settings_popover->set_permissions(view().site_permission_rows());
}

void Tab::show_private_session_popover()
{
    if (m_window->is_private() == WebView::IsPrivate::No)
        return;

    if (!m_private_session_popover) {
        m_private_session_popover = new MessagePopover(this, PRIVATE_SESSION_POPOVER_WIDTH, "Start a fresh private session?",
            "This closes all private windows and deletes history and other site data from the current private browsing session.",
            "Cancel", "Start fresh session");
        m_private_session_popover->on_accept = [] {
            Application::the().restart_private_browsing_session();
        };
    }

    m_private_session_popover->update_chrome_style(palette());
    position_private_session_popover();
    m_private_session_popover->show();
    position_private_session_popover();
    m_private_session_popover->raise();
}

void Tab::position_private_session_popover()
{
    if (!m_private_session_popover)
        return;

    m_private_session_popover->adjustSize();

    move_popover_below(*m_private_session_popover, m_window->ladybird_button(), PopoverAlignment::Leading, m_window->ladybird_button().tile_rect());
}

void Tab::show_restore_session_card(size_t tab_count, Function<void()> on_restore)
{
    if (m_restore_session_card)
        m_restore_session_card->deleteLater();
    m_restore_session_card = new RestoreSessionCard(*this, *m_view, tab_count);
    m_view->add_floating_surface(*m_restore_session_card);
    m_restore_session_card->on_restore = AK::move(on_restore);
    m_restore_session_card->show();
}

void Tab::show_find_in_page()
{
    m_find_in_page->setVisible(true);
    m_find_in_page->focus_search_field();
}

void Tab::find_previous()
{
    m_find_in_page->find_previous();
}

void Tab::find_next()
{
    m_find_in_page->find_next();
}

void Tab::update_bookmarks_bar_visibility()
{
    auto show_bookmarks_bar = Application::settings().appearance().show_bookmarks_bar;

    if (show_bookmarks_bar == WebView::ShowBookmarksBar::OnHover) {
        // Hovering must not resize the page or move the sidebar. Keep the bar out of both layouts and overlay the view.
        static_cast<QBoxLayout*>(layout())->removeWidget(m_bookmarks_bar_container);
        if (m_bookmarks_bar_container->parentWidget() != this)
            m_bookmarks_bar_container->setParent(this);
    } else if (auto* tab_layout = static_cast<QBoxLayout*>(layout()); tab_layout->indexOf(m_bookmarks_bar_container) == -1) {
        // An always-visible bar sits above the page, so that it is beside a collapsed sidebar rather than above it.
        m_bookmarks_bar_container->setParent(this);
        tab_layout->insertWidget(tab_layout->indexOf(m_view), m_bookmarks_bar_container);
    }

    update_bookmarks_bar_hover();
    update_bookmarks_bar_geometry();
}

bool Tab::has_bookmarks_bar_in_page() const
{
    // A tab that hasn't loaded has no bookmarks bar yet.
    if (!is_loaded())
        return false;
    return m_bookmarks_bar_container->parentWidget() == this && !m_bookmarks_bar_container->isHidden();
}

bool Tab::can_expand_bookmarks_bar_on_hover() const
{
    return Application::settings().appearance().show_bookmarks_bar == WebView::ShowBookmarksBar::OnHover
        && isVisible() && m_window->toolbar().isVisible() && !m_window->isFullScreen()
        && !m_location_edit->property("autocompleteOpen").toBool();
}

bool Tab::cursor_is_over_bookmarks_bar_hover_region() const
{
    // Menus are separate windows, so QWidget::isAncestorOf() does not follow them back to the toolbar.
    for (auto* popup = QApplication::activePopupWidget(); popup; popup = popup->parentWidget()) {
        if (popup == m_bookmarks_bar)
            return true;
    }

    // NB: The page can be presented by a native window of its own, which takes the pointer from the browser window.
    auto* widget_under_cursor = QApplication::widgetAt(QCursor::pos());
    if (!m_window->underMouse() && !(widget_under_cursor && widget_under_cursor->window() == m_window))
        return false;

    auto cursor_position = QCursor::pos();
    auto contains_cursor = [&](QWidget const& widget) {
        return widget.isVisible() && QRect { widget.mapToGlobal(QPoint {}), widget.size() }.contains(cursor_position);
    };
    return contains_cursor(m_window->toolbar()) || contains_cursor(*m_bookmarks_bar_container);
}

void Tab::update_bookmarks_bar_hover()
{
    if (!can_expand_bookmarks_bar_on_hover() || !cursor_is_over_bookmarks_bar_hover_region()) {
        set_bookmarks_bar_hover_expanded(false);
        return;
    }

    if (!m_bookmarks_bar_hover_expanded && !m_bookmarks_bar_hover_expand_timer->isActive()) {
        auto delay = Application::settings().config_variable_as_integer<int>(WebView::ConfigVariableID::VerticalTabsHoverExpandDelay);
        m_bookmarks_bar_hover_expand_timer->start(delay);
    }
}

void Tab::set_bookmarks_bar_hover_expanded(bool expanded)
{
    m_bookmarks_bar_hover_expand_timer->stop();
    expanded &= can_expand_bookmarks_bar_on_hover();
    if (m_bookmarks_bar_hover_expanded == expanded)
        return;

    m_bookmarks_bar_hover_expanded = expanded;
    m_bookmarks_bar_hover_expand_animation->stop();

    auto duration = expanded && !SystemAppearance::prefers_reduced_motion()
        ? Application::settings().config_variable_as_integer<int>(WebView::ConfigVariableID::VerticalTabsHoverExpandAnimationDuration)
        : 0;
    m_bookmarks_bar_hover_expand_progress = expanded && duration == 0 ? 1.0 : 0.0;

    if (expanded)
        m_bookmarks_bar_hover_collapse_timer->start();
    else
        m_bookmarks_bar_hover_collapse_timer->stop();

    update_bookmarks_bar_geometry();

    if (expanded && duration > 0) {
        m_bookmarks_bar_hover_expand_animation->setDuration(duration);
        m_bookmarks_bar_hover_expand_animation->start();
    }
}

void Tab::update_bookmarks_bar_geometry()
{
    auto show_bookmarks_bar = Application::settings().appearance().show_bookmarks_bar;
    auto on_hover = show_bookmarks_bar == WebView::ShowBookmarksBar::OnHover;
    auto visible = !m_window->isFullScreen()
        && (show_bookmarks_bar == WebView::ShowBookmarksBar::Always
            || (show_bookmarks_bar == WebView::ShowBookmarksBar::OnHover && m_bookmarks_bar_hover_expanded));

    auto full_height = m_bookmarks_bar->sizeHint().height();
    auto height = on_hover
        ? min(qRound(full_height * m_bookmarks_bar_hover_expand_progress), m_view->height())
        : full_height;

    // Clip the full-height toolbar as it slides down, keeping its buttons at their normal size throughout the reveal.
    m_bookmarks_bar_container->setFixedHeight(height);
    if (on_hover)
        m_bookmarks_bar_container->setGeometry(m_view->x(), m_view->y(), m_view->width(), height);
    m_bookmarks_bar->setGeometry(0, height - full_height, m_bookmarks_bar_container->width(), full_height);
    m_view->set_bookmarks_bar_overlay_height(on_hover && visible ? height : 0);
    m_bookmarks_bar_container->setVisible(visible);
    if (on_hover && visible)
        m_bookmarks_bar_container->raise();

#if defined(AK_OS_MACOS)
    ensure_appkit_window_is_exposed(*m_bookmarks_bar_container);
#endif

    if (m_window->current_tab() == this)
        m_window->toolbar().set_bookmarks_bar_below(visible);
}

void Tab::request_close()
{
    m_window->cancel_window_close_preflight();

    // A tab that hasn't loaded has no page to ask, so it closes at once.
    if (!is_loaded()) {
        m_window->definitely_close_tab(tab_index());
        return;
    }

    m_suppress_javascript_dialogs_until_navigation = true;
    m_preserve_close_retry_state_on_dialog_dismissal = m_already_requested_close;
    m_javascript_dialog->dismiss();
    m_preserve_close_retry_state_on_dialog_dismissal = false;

    if (!view().needs_beforeunload_check()) {
        auto request_close = view().prepare_for_immediate_close();
        if (m_window->definitely_close_tab(tab_index()))
            Core::deferred_invoke(AK::move(request_close));
        return;
    }

    // Prevent closing on first request so WebContent can cleanly shutdown (e.g. asking if the user is sure they want
    // to leave, closing WebSocket connections, etc.)
    if (!m_already_requested_close) {
        m_already_requested_close = true;
        view().request_close();
        return;
    }

    // If the user has already requested a close, then respect the user's request and just close the tab.
    // For example, the WebContent process may not be responding.
    m_window->definitely_close_tab(tab_index());
}

void Tab::request_window_close_preflight(Function<void(bool)> on_complete)
{
    if (!is_loaded()) {
        on_complete(true);
        return;
    }

    m_suppress_javascript_dialogs_until_navigation = true;

    if (m_javascript_dialog->type() != JavaScriptDialog::Type::BeforeUnload)
        m_javascript_dialog->dismiss();

    if (!view().needs_beforeunload_check()) {
        on_complete(true);
        return;
    }

    auto weak_this = QPointer<Tab>(this);
    view().request_close_preflight([weak_this, on_complete = AK::move(on_complete)](bool approved) mutable {
        if (!weak_this) {
            on_complete(true);
            return;
        }
        on_complete(approved);
    });
}

void Tab::cancel_window_close_preflight()
{
    if (!is_loaded())
        return;

    if (m_javascript_dialog->type() == JavaScriptDialog::Type::BeforeUnload)
        m_javascript_dialog->dismiss();
    m_already_requested_close = false;
    m_suppress_javascript_dialogs_until_navigation = false;
}

Function<void()> Tab::prepare_for_window_close()
{
    if (!is_loaded())
        return [] { };

    m_window_is_closing = true;
    m_suppress_javascript_dialogs_until_navigation = true;
    m_javascript_dialog->dismiss();
    return view().prepare_for_close_without_prompting();
}

}
