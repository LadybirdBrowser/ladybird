/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWebView/Application.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/Menu.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/WindowControlButton.h>

#include <QColorDialog>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr auto WINDOW_DRAG_REGION_PROPERTY = "LadybirdWindowDragRegion";

class HamburgerButton final : public QToolButton {
public:
    using QToolButton::QToolButton;

protected:
    virtual void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton)
            show_menu();
        else
            QToolButton::mousePressEvent(event);
    }

    virtual void keyPressEvent(QKeyEvent* event) override
    {
        if (first_is_one_of(event->key(), Qt::Key_Select, Qt::Key_Space))
            show_menu();
        else
            QToolButton::keyPressEvent(event);
    }

private:
    void show_menu()
    {
        auto* menu = this->menu();
        VERIFY(menu);

        auto bottom_right = mapToGlobal(rect().bottomRight());
        auto menu_width = menu->sizeHint().width();

        menu->popup(QPoint { bottom_right.x() - menu_width, bottom_right.y() });
    }
};

class DownloadsButton final : public QToolButton {
public:
    using QToolButton::QToolButton;

    void set_progress(Optional<double> progress)
    {
        auto normalized_progress = progress.map([](auto value) {
            if (value < 0.0)
                return 0.0;
            if (value > 1.0)
                return 1.0;
            return value;
        });

        if (m_progress.has_value() == normalized_progress.has_value()) {
            if (!normalized_progress.has_value())
                return;
            if (AK::abs(*m_progress - *normalized_progress) < 0.005)
                return;
        }

        m_progress = AK::move(normalized_progress);
        update();
    }

    void set_progress_icon(QIcon icon)
    {
        m_progress_icon = icon;
        update();
    }

protected:
    virtual void paintEvent(QPaintEvent* event) override
    {
        QToolButton::paintEvent(event);

        if (!m_progress.has_value())
            return;

        auto progress = *m_progress;
        if (progress < 0.0)
            progress = 0.0;
        if (progress > 1.0)
            progress = 1.0;

        auto const icon_size = this->iconSize();
        auto icon_rect = QRectF {
            (static_cast<qreal>(width()) - icon_size.width()) / 2.0,
            (static_cast<qreal>(height()) - icon_size.height()) / 2.0,
            static_cast<qreal>(icon_size.width()),
            static_cast<qreal>(icon_size.height())
        };
        auto fill_rect = QRectF {
            icon_rect.left(),
            icon_rect.top(),
            icon_rect.width(),
            icon_rect.height() * progress
        };

        QPainter painter(this);
        painter.setClipRect(fill_rect);
        auto pixmap = m_progress_icon.pixmap(icon_size, devicePixelRatioF(), isEnabled() ? QIcon::Normal : QIcon::Disabled);
        painter.drawPixmap(icon_rect.toRect(), pixmap);
    }

private:
    Optional<double> m_progress;
    QIcon m_progress_icon;
};

static QToolButton* create_toolbar_button(QWidget& parent, QAction& action)
{
    auto* button = new QToolButton(&parent);
    button->setDefaultAction(&action);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize({ 20, 20 });
    button->setFixedSize(36, 36);

    // FIXME: In Menu.cpp, we set the initial visibility of the action before we've associated it with this QToolButton.
    //        It would be nicer if we didn't have to do this here.
    button->setVisible(action.isVisible());

    return button;
}

static void populate_navigation_history_menu(QMenu& menu, WebContentView& view, int direction)
{
    static constexpr int const MENU_ICON_SIZE = 16;

    menu.clear();

    for (auto const& item : view.session_history_traversal_menu_items(direction)) {
        auto* action = menu.addAction(qstring_from_ak_string(item.title));
        action->setToolTip(qstring_from_ak_string(item.url));
        if (item.favicon_base64_png.has_value())
            action->setIcon(icon_from_base64_png(*item.favicon_base64_png, MENU_ICON_SIZE));
        else
            action->setIcon(create_chrome_icon(ChromeIcon::Globe, menu.palette()));
        QObject::connect(action, &QAction::triggered, &view, [&view, delta = item.delta] {
            (void)view.traverse_the_history_by_delta(delta);
        });
    }
}

static QToolButton* create_navigation_history_toolbar_button(QWidget& parent, QAction& action, WebContentView& view, int direction)
{
    auto* button = create_toolbar_button(parent, action);
    auto* menu = new QMenu(button);
    QObject::connect(menu, &QMenu::aboutToShow, button, [menu, &view, direction] {
        populate_navigation_history_menu(*menu, view, direction);
    });
    button->setMenu(menu);
    button->setPopupMode(QToolButton::DelayedPopup);
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(button, &QToolButton::customContextMenuRequested, button, [button, menu, &view, direction](QPoint const&) {
        populate_navigation_history_menu(*menu, view, direction);
        if (!menu->isEmpty())
            button->showMenu();
    });
    return button;
}

static constexpr int TOOLBAR_HORIZONTAL_MARGIN = 12;
static constexpr int TOOLBAR_VERTICAL_MARGIN = 2;
static constexpr int TOOLBAR_MACOS_TRAFFIC_LIGHTS_CONTROL_GAP = 22;
static constexpr int TOOLBAR_SIDEBAR_TOGGLE_NAVIGATION_GAP = 8;
static constexpr int TOOLBAR_LOCATION_EDIT_SIDE_GAP = 32;
static constexpr int TOOLBAR_WINDOW_CONTROLS_RIGHT_MARGIN = 4;
static constexpr int DOWNLOADS_POPOVER_WIDTH = 380;
static constexpr int DOWNLOADS_POPOVER_MAX_HEIGHT = 360;
static constexpr int DOWNLOADS_POPOVER_MAX_VISIBLE_DOWNLOADS = 5;

static QString download_count_text(size_t count, char const* singular, char const* plural)
{
    return QString("%1 %2").arg(count).arg(count == 1 ? singular : plural);
}

static QString download_percent_text(double progress)
{
    if (progress < 0.0)
        progress = 0.0;
    if (progress > 1.0)
        progress = 1.0;

    auto percent = static_cast<int>(progress * 100.0);
    return QString("%1%").arg(percent);
}

static QString download_size_text(u64 size)
{
    constexpr double kibibyte = 1024.0;
    constexpr double mebibyte = kibibyte * 1024.0;
    constexpr double gibibyte = mebibyte * 1024.0;

    auto size_as_double = static_cast<double>(size);
    if (size_as_double < kibibyte)
        return QString("%1 B").arg(size);
    if (size_as_double < mebibyte)
        return QString("%1 KB").arg(size_as_double / kibibyte, 0, 'f', 1);
    if (size_as_double < gibibyte)
        return QString("%1 MB").arg(size_as_double / mebibyte, 0, 'f', 1);
    return QString("%1 GB").arg(size_as_double / gibibyte, 0, 'f', 1);
}

static Optional<double> download_progress(WebView::FileDownloader::Download const& download)
{
    if (!download.total_size.has_value() || *download.total_size == 0)
        return {};

    return static_cast<double>(min(download.downloaded_size, *download.total_size)) / static_cast<double>(*download.total_size);
}

static QString download_status_text(WebView::FileDownloader::Download const& download)
{
    using DownloadStatus = WebView::FileDownloader::DownloadStatus;

    switch (download.status) {
    case DownloadStatus::InProgress:
        if (auto progress = download_progress(download); progress.has_value()) {
            return QString("%1 of %2 - %3")
                .arg(download_size_text(download.downloaded_size))
                .arg(download_size_text(*download.total_size))
                .arg(download_percent_text(*progress));
        }
        return QString("%1 downloaded").arg(download_size_text(download.downloaded_size));
    case DownloadStatus::Completed:
        return QString("Completed - %1").arg(download_size_text(download.downloaded_size));
    case DownloadStatus::Canceled:
        return "Canceled";
    case DownloadStatus::Failed:
        if (download.error.has_value() && !download.error->is_empty())
            return QString("Failed - %1").arg(qstring_from_ak_string(*download.error));
        return "Failed";
    }
    VERIFY_NOT_REACHED();
}

static QString downloads_popover_style_sheet(QPalette const& palette)
{
    auto surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface(palette));
    auto recessed_surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface_recessed(palette));
    auto hover_surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface_hover(palette));
    auto border = ChromeStyle::style_sheet_color(ChromeStyle::chrome_border(palette));
    auto text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_text(palette));
    auto muted_text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_muted_text(palette));
    auto accent = ChromeStyle::style_sheet_color(ChromeStyle::chrome_accent(palette));

    return qformatted(R"(
QFrame#LadybirdDownloadsPopover {{
    color: {4};
    background: {0};
    border: 1px solid {3};
    border-radius: 8px;
}}

QScrollArea#LadybirdDownloadsPopoverScroll,
QWidget#LadybirdDownloadsPopoverRows {{
    background: transparent;
    border: 0;
}}

QLabel#LadybirdDownloadsPopoverTitle,
QLabel#LadybirdDownloadFileName {{
    color: {4};
    font-weight: 600;
}}

QLabel#LadybirdDownloadStatus,
QLabel#LadybirdDownloadsEmpty {{
    color: {5};
}}

QFrame#LadybirdDownloadRow {{
    background: {0};
    border: 1px solid {3};
    border-radius: 6px;
}}

QFrame#LadybirdDownloadRow:hover {{
    background: {2};
}}

QProgressBar#LadybirdDownloadProgress {{
    background: {1};
    border: 0;
    border-radius: 2px;
    min-height: 4px;
    max-height: 4px;
}}

QProgressBar#LadybirdDownloadProgress::chunk {{
    background: {6};
    border-radius: 2px;
}}
)",
        surface, recessed_surface, hover_surface, border, text, muted_text, accent);
}

class ElidedLabel final : public QLabel {
public:
    explicit ElidedLabel(QString text, Qt::TextElideMode elide_mode, QWidget* parent = nullptr)
        : QLabel(text, parent)
        , m_elide_mode(elide_mode)
    {
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    virtual QSize sizeHint() const override
    {
        auto size = QLabel::sizeHint();
        size.setWidth(0);
        return size;
    }

    virtual QSize minimumSizeHint() const override
    {
        return { 0, QLabel::minimumSizeHint().height() };
    }

protected:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setFont(font());
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(rect(), alignment() | Qt::TextSingleLine, fontMetrics().elidedText(text(), m_elide_mode, width()));
    }

private:
    Qt::TextElideMode m_elide_mode { Qt::ElideRight };
};

class DownloadRow final : public QFrame {
public:
    explicit DownloadRow(WebView::FileDownloader::Download const& download, QWidget* parent)
        : QFrame(parent)
        , m_download_id(download.id)
    {
        setObjectName("LadybirdDownloadRow");
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto* row_layout = new QHBoxLayout(this);
        row_layout->setContentsMargins(10, 8, 10, 8);
        row_layout->setSpacing(8);

        auto* details_layout = new QVBoxLayout;
        details_layout->setContentsMargins(0, 0, 0, 0);
        details_layout->setSpacing(4);

        m_file_name_label = new ElidedLabel({}, Qt::ElideRight, this);
        m_file_name_label->setObjectName("LadybirdDownloadFileName");
        details_layout->addWidget(m_file_name_label);

        m_status_label = new ElidedLabel({}, Qt::ElideRight, this);
        m_status_label->setObjectName("LadybirdDownloadStatus");
        details_layout->addWidget(m_status_label);

        m_progress_bar = new QProgressBar(this);
        m_progress_bar->setObjectName("LadybirdDownloadProgress");
        m_progress_bar->setRange(0, 1000);
        m_progress_bar->setTextVisible(false);
        details_layout->addWidget(m_progress_bar);

        row_layout->addLayout(details_layout, 1);

        m_cancel_button = new QPushButton("Cancel", this);
        m_cancel_button->setFocusPolicy(Qt::NoFocus);
        QObject::connect(m_cancel_button, &QPushButton::clicked, this, [this] {
            if (on_cancel_download)
                on_cancel_download(m_download_id);
        });
        row_layout->addWidget(m_cancel_button, 0, Qt::AlignTop);

        (void)update_download(download);
    }

    u64 id() const { return m_download_id; }

    bool update_download(WebView::FileDownloader::Download const& download)
    {
        VERIFY(download.id == m_download_id);

        m_file_name_label->setText(qstring_from_ak_string(download.destination.basename()));
        m_file_name_label->setToolTip(qstring_from_ak_string(download.destination.string()));

        auto status_text = download_status_text(download);
        m_status_label->setText(status_text);
        m_status_label->setToolTip(status_text);

        bool geometry_changed = false;
        auto progress = download_progress(download);
        auto show_progress = download.status == WebView::FileDownloader::DownloadStatus::InProgress && progress.has_value();
        auto progress_bar_is_visible = !m_progress_bar->isHidden();
        if (progress_bar_is_visible != show_progress) {
            m_progress_bar->setVisible(show_progress);
            geometry_changed = true;
        }
        if (progress.has_value())
            m_progress_bar->setValue(static_cast<int>(*progress * 1000.0));

        auto should_show_cancel = download.status == WebView::FileDownloader::DownloadStatus::InProgress;
        auto cancel_button_is_visible = !m_cancel_button->isHidden();
        if (cancel_button_is_visible != should_show_cancel) {
            m_cancel_button->setVisible(should_show_cancel);
            geometry_changed = true;
        }

        return geometry_changed;
    }

    Function<void(u64)> on_cancel_download;

private:
    u64 m_download_id { 0 };
    ElidedLabel* m_file_name_label { nullptr };
    ElidedLabel* m_status_label { nullptr };
    QProgressBar* m_progress_bar { nullptr };
    QPushButton* m_cancel_button { nullptr };
};

class DownloadsPopover final : public QFrame {
public:
    explicit DownloadsPopover(QWidget* parent)
        : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setObjectName("LadybirdDownloadsPopover");
#if defined(AK_OS_MACOS)
        setAttribute(Qt::WA_NativeWindow);
#endif
        setFrameShape(QFrame::StyledPanel);
        setFrameShadow(QFrame::Raised);
        setAutoFillBackground(true);
        setFixedWidth(DOWNLOADS_POPOVER_WIDTH);

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 12);
        layout->setSpacing(8);

        auto* title = new QLabel("Downloads", this);
        title->setObjectName("LadybirdDownloadsPopoverTitle");
        layout->addWidget(title);

        m_empty_label = new QLabel("No downloads", this);
        m_empty_label->setObjectName("LadybirdDownloadsEmpty");
        m_empty_label->setAlignment(Qt::AlignCenter);
        m_empty_label->setMinimumHeight(80);
        layout->addWidget(m_empty_label);

        m_scroll_area = new QScrollArea(this);
        m_scroll_area->setObjectName("LadybirdDownloadsPopoverScroll");
        m_scroll_area->setFrameShape(QFrame::NoFrame);
        m_scroll_area->setWidgetResizable(true);
        m_scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        m_rows_widget = new QWidget(m_scroll_area);
        m_rows_widget->setObjectName("LadybirdDownloadsPopoverRows");
        m_rows_widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_rows_layout = new QVBoxLayout(m_rows_widget);
        m_rows_layout->setContentsMargins(0, 0, 0, 0);
        m_rows_layout->setSpacing(6);
        m_scroll_area->setWidget(m_rows_widget);
        layout->addWidget(m_scroll_area);

        auto* show_all_button = new QPushButton("Show All Downloads", this);
        QObject::connect(show_all_button, &QPushButton::clicked, this, [this] {
            if (on_open_all_downloads)
                on_open_all_downloads();
        });
        layout->addWidget(show_all_button);
    }

    void update_chrome_style(QPalette const& palette)
    {
        setPalette(palette);
        setStyleSheet(downloads_popover_style_sheet(palette));
    }

    bool set_downloads(ReadonlySpan<WebView::FileDownloader::Download> downloads)
    {
        bool geometry_changed = false;
        Vector<WebView::FileDownloader::Download const*> visible_downloads;
        visible_downloads.ensure_capacity(min(downloads.size(), DOWNLOADS_POPOVER_MAX_VISIBLE_DOWNLOADS));

        for (size_t i = downloads.size(); i > 0 && visible_downloads.size() < DOWNLOADS_POPOVER_MAX_VISIBLE_DOWNLOADS; --i)
            visible_downloads.append(&downloads[i - 1]);

        if (!rows_match(visible_downloads)) {
            rebuild_rows(visible_downloads);
            geometry_changed = true;
        } else {
            for (size_t i = 0; i < visible_downloads.size(); ++i) {
                if (m_download_rows[i]->update_download(*visible_downloads[i]))
                    geometry_changed = true;
            }
        }

        auto is_empty = visible_downloads.is_empty();
        auto empty_label_is_visible = !m_empty_label->isHidden();
        if (empty_label_is_visible != is_empty) {
            m_empty_label->setVisible(is_empty);
            geometry_changed = true;
        }
        auto scroll_area_is_visible = !m_scroll_area->isHidden();
        if (scroll_area_is_visible == is_empty) {
            m_scroll_area->setVisible(!is_empty);
            geometry_changed = true;
        }

        return geometry_changed;
    }

    Function<void(u64)> on_cancel_download;
    Function<void()> on_open_all_downloads;

private:
    bool rows_match(Vector<WebView::FileDownloader::Download const*> const& downloads) const
    {
        if (m_download_rows.size() != downloads.size())
            return false;

        for (size_t i = 0; i < downloads.size(); ++i) {
            if (m_download_rows[i]->id() != downloads[i]->id)
                return false;
        }

        return true;
    }

    void rebuild_rows(Vector<WebView::FileDownloader::Download const*> const& downloads)
    {
        clear_rows();
        m_download_rows.ensure_capacity(downloads.size());

        for (auto* download : downloads) {
            auto* row = new DownloadRow(*download, m_rows_widget);
            row->on_cancel_download = [this](u64 id) {
                if (on_cancel_download)
                    on_cancel_download(id);
            };
            m_rows_layout->addWidget(row);
            m_download_rows.append(row);
        }
    }

    void clear_rows()
    {
        while (auto* item = m_rows_layout->takeAt(0)) {
            if (auto* widget = item->widget())
                delete widget;
            delete item;
        }
        m_download_rows.clear();
    }

    QLabel* m_empty_label { nullptr };
    QScrollArea* m_scroll_area { nullptr };
    QWidget* m_rows_widget { nullptr };
    QVBoxLayout* m_rows_layout { nullptr };
    Vector<DownloadRow*> m_download_rows;
};

Tab::Tab(BrowserWindow* window, RefPtr<WebView::WebContentClient> parent_client, size_t page_index)
    : QWidget(window)
    , m_window(window)
{
    auto& application = WebView::Application::the();

    auto* tab_layout = new QBoxLayout(QBoxLayout::Direction::TopToBottom, this);
    tab_layout->setSpacing(0);
    tab_layout->setContentsMargins(0, 0, 0, 0);

    auto view_initial_state = WebContentViewInitialState {
        .maximum_frames_per_second = window->refresh_rate(),
        .display_id = window->display_id(),
    };

    m_view = new WebContentView(this, parent_client, page_index, AK::move(view_initial_state));
    m_find_in_page = new FindInPageWidget(this, m_view);
    m_find_in_page->setVisible(false);

    m_toolbar_container = new QWidget(this);
    m_toolbar_container->setObjectName("LadybirdToolbarContainer");
    m_toolbar_container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName("LadybirdNavigationToolbar");
    m_toolbar->setFixedHeight(browser_chrome_layout_policy().toolbar_height);
    m_toolbar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);

    auto* toolbar_container_layout = new QVBoxLayout(m_toolbar_container);
    toolbar_container_layout->setSpacing(0);
    toolbar_container_layout->setContentsMargins(0, 0, 0, 0);

    auto* toolbar_layout = new QHBoxLayout(m_toolbar);
    toolbar_layout->setSpacing(6);
    toolbar_layout->setContentsMargins(TOOLBAR_HORIZONTAL_MARGIN, TOOLBAR_VERTICAL_MARGIN, TOOLBAR_HORIZONTAL_MARGIN, TOOLBAR_VERTICAL_MARGIN);

    m_location_edit = new LocationEdit(this);
    m_bookmarks_bar = new BookmarksBar(this);
    m_loading_animation_timer = new QTimer(this);
    m_loading_animation_timer->setInterval(80);

    m_hover_label = new HyperlinkLabel(this);
    m_hover_label->hide();
    m_hover_label->setContentsMargins(4, 2, 4, 2);
    m_hover_label->setAutoFillBackground(true);

    QObject::connect(m_hover_label, &HyperlinkLabel::mouse_entered, [this] {
        update_hover_label();
    });
    QObject::connect(m_loading_animation_timer, &QTimer::timeout, this, [this] {
        m_loading_animation_frame = (m_loading_animation_frame + 1) % 12;
        update_tab_icon();
    });

    auto* focus_location_editor_action = new QAction("Edit Location", this);
    focus_location_editor_action->setShortcuts({ QKeySequence("Ctrl+L"), QKeySequence("Alt+D") });
    addAction(focus_location_editor_action);

    toolbar_container_layout->addWidget(m_toolbar);
    toolbar_container_layout->addWidget(m_bookmarks_bar);
    tab_layout->addWidget(m_view);
    tab_layout->addWidget(m_find_in_page);

    m_hamburger_button = new HamburgerButton(m_toolbar);
    m_hamburger_button->setText("Show &Menu");
    m_hamburger_button->setToolTip("Show Menu");
    m_hamburger_button->setIcon(create_chrome_icon(ChromeIcon::Menu, palette()));
    m_hamburger_button->setIconSize({ 20, 20 });
    m_hamburger_button->setFixedSize(36, 36);
    m_hamburger_button->setAutoRaise(true);
    m_hamburger_button->setFocusPolicy(Qt::NoFocus);
    m_hamburger_button->setPopupMode(QToolButton::InstantPopup);
    m_hamburger_button->setMenu(&m_window->hamburger_menu());
    connect_hamburger_menu();

    m_navigate_back_action = create_application_action(*this, view().navigate_back_action());
    m_navigate_forward_action = create_application_action(*this, view().navigate_forward_action());
    m_reload_action = create_application_action(*this, application.reload_action());
    m_toggle_vertical_tabs_expanded_action = create_application_action(*this, application.toggle_vertical_tabs_expanded_action());
    m_open_downloads_page_action = create_application_action(*this, application.open_downloads_page_action());

    m_toolbar_window_controls_separator = new QWidget(m_toolbar);
    m_toolbar_window_controls_separator->setObjectName("LadybirdToolbarWindowControlsSeparator");
    m_toolbar_window_controls_separator->setFixedSize(1, 22);

    auto window_control_buttons = create_window_control_buttons(*m_toolbar, "LadybirdToolbarWindowControls", { 16, 16 }, { 38, 38 });
    m_toolbar_window_controls = window_control_buttons.container;
    m_minimize_window_button = window_control_buttons.minimize;
    m_maximize_window_button = window_control_buttons.maximize;
    m_close_window_button = window_control_buttons.close;

    QObject::connect(m_minimize_window_button, &QToolButton::clicked, this, [this] {
        m_window->showMinimized();
    });
    QObject::connect(m_maximize_window_button, &QToolButton::clicked, this, [this] {
        if (m_window->isMaximized())
            m_window->showNormal();
        else
            m_window->showMaximized();
    });
    QObject::connect(m_close_window_button, &QToolButton::clicked, this, [this] {
        m_window->close();
    });

    recreate_toolbar_icons();

    m_page_context_menu = create_context_menu(*this, view(), view().page_context_menu());
    m_link_context_menu = create_context_menu(*this, view(), view().link_context_menu());
    m_image_context_menu = create_context_menu(*this, view(), view().image_context_menu());
    m_media_context_menu = create_context_menu(*this, view(), view().media_context_menu());

    auto* navigation_button_cluster = new QWidget(m_toolbar);
    navigation_button_cluster->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    auto* navigation_button_layout = new QHBoxLayout(navigation_button_cluster);
    navigation_button_layout->setSpacing(2);
    navigation_button_layout->setContentsMargins(0, 0, 0, 0);
    m_left_toggle_vertical_tabs_expanded_button = create_toolbar_button(*navigation_button_cluster, *m_toggle_vertical_tabs_expanded_action);
    navigation_button_layout->addWidget(m_left_toggle_vertical_tabs_expanded_button);
    m_sidebar_toggle_navigation_spacer = new QSpacerItem(0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    navigation_button_layout->addItem(m_sidebar_toggle_navigation_spacer);
    navigation_button_layout->addWidget(create_navigation_history_toolbar_button(*navigation_button_cluster, *m_navigate_back_action, view(), -1));
    navigation_button_layout->addWidget(create_navigation_history_toolbar_button(*navigation_button_cluster, *m_navigate_forward_action, view(), 1));
    navigation_button_layout->addWidget(create_toolbar_button(*navigation_button_cluster, *m_reload_action));

    if (use_left_traffic_light_window_controls()) {
        toolbar_layout->addWidget(m_toolbar_window_controls, 0, Qt::AlignVCenter);
        m_toolbar_window_controls_spacer = new QSpacerItem(TOOLBAR_MACOS_TRAFFIC_LIGHTS_CONTROL_GAP, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
        toolbar_layout->addItem(m_toolbar_window_controls_spacer);
    }
    auto* location_edit_container = new QWidget(m_toolbar);
    location_edit_container->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    auto* location_edit_layout = new QHBoxLayout(location_edit_container);
    location_edit_layout->setSpacing(0);
    location_edit_layout->setContentsMargins(TOOLBAR_LOCATION_EDIT_SIDE_GAP, 0, TOOLBAR_LOCATION_EDIT_SIDE_GAP, 0);

    toolbar_layout->addWidget(navigation_button_cluster, 0, Qt::AlignTop);
    m_location_edit->set_trailing_action(create_application_action(*m_location_edit, view().toggle_bookmark_action()));
    m_location_edit->set_zoom_action(create_application_action(*m_location_edit, view().reset_zoom_action(), IncludeActionIcon::No));
    location_edit_layout->addWidget(m_location_edit);
    toolbar_layout->addWidget(location_edit_container, 1);
    m_right_toggle_vertical_tabs_expanded_button = create_toolbar_button(*m_toolbar, *m_toggle_vertical_tabs_expanded_action);
    toolbar_layout->addWidget(m_right_toggle_vertical_tabs_expanded_button, 0, Qt::AlignTop);
    m_downloads_button = new DownloadsButton(m_toolbar);
    m_downloads_button->setText("Downloads");
    m_downloads_button->setAutoRaise(true);
    m_downloads_button->setFocusPolicy(Qt::NoFocus);
    m_downloads_button->setIconSize({ 20, 20 });
    m_downloads_button->setFixedSize(36, 36);
    m_downloads_button->setVisible(m_open_downloads_page_action->isVisible());
    QObject::connect(m_downloads_button, &QToolButton::clicked, this, [this] {
        show_downloads_popover();
    });
    toolbar_layout->addWidget(m_downloads_button, 0, Qt::AlignTop);
    toolbar_layout->addWidget(m_hamburger_button, 0, Qt::AlignTop);
    if (use_right_custom_window_controls()) {
        toolbar_layout->addWidget(m_toolbar_window_controls_separator, 0, Qt::AlignVCenter);
        toolbar_layout->addWidget(m_toolbar_window_controls, 0, Qt::AlignVCenter);
    }

    update_chrome_style();
    update_downloads_button();
    set_toolbar_window_controls_visible(false);

    view().on_activate_tab = [this] {
        m_window->activate_tab(tab_index());
    };

    view().on_close = [this] {
        m_window->definitely_close_tab(tab_index());
    };

    view().on_link_hover = [this](auto const& url) {
        m_hover_label->setText(qstring_from_ak_string(url.to_byte_string()));
        update_hover_label();
        m_hover_label->show();
    };

    view().on_link_unhover = [this]() {
        m_hover_label->hide();
    };

    view().on_load_start = [this](URL::URL const& url, bool) {
        auto url_serialized = qstring_from_ak_string(url.serialize());

        m_title = url_serialized;
        update_tab_title();

        m_favicon = {};
        set_loading(true);

        m_location_edit->set_url(url);
    };

    view().on_load_finish = [this](auto const&) {
        set_loading(false);
    };

    view().on_web_content_crashed = [this] {
        set_loading(false);
    };

    view().on_url_change = [this](auto const& url) {
        m_location_edit->set_url(url);
    };

    QObject::connect(m_location_edit, &QLineEdit::returnPressed, this, &Tab::location_edit_return_pressed);

    view().on_title_change = [this](auto const& title) {
        m_title = qstring_from_utf16_string(title);
        update_tab_title();
    };

    view().on_favicon_change = [this](auto const& bitmap) {
        auto qimage = QImage(bitmap.scanline_u8(0), bitmap.width(), bitmap.height(), QImage::Format_ARGB32);
        if (qimage.isNull())
            return;
        auto qpixmap = QPixmap::fromImage(qimage);
        if (qpixmap.isNull())
            return;

        m_favicon = qpixmap;
        update_tab_icon();
    };

    view().on_request_alert = [this](auto const& message) {
        m_dialog = new QMessageBox(QMessageBox::Icon::Warning, "Ladybird", qstring_from_ak_string(message), QMessageBox::StandardButton::Ok, &view());

        QObject::connect(m_dialog, &QDialog::finished, this, [this]() {
            view().alert_closed();
            m_dialog = nullptr;
        });

        m_dialog->open();
    };

    view().on_request_confirm = [this](auto const& message) {
        m_dialog = new QMessageBox(QMessageBox::Icon::Question, "Ladybird", qstring_from_ak_string(message), QMessageBox::StandardButton::Ok | QMessageBox::StandardButton::Cancel, &view());

        QObject::connect(m_dialog, &QDialog::finished, this, [this](auto result) {
            view().confirm_closed(result == QMessageBox::StandardButton::Ok || result == QDialog::Accepted);
            m_dialog = nullptr;
        });

        m_dialog->open();
    };

    view().on_request_prompt = [this](auto const& message, auto const& default_) {
        m_dialog = new QInputDialog(&view());

        auto& dialog = static_cast<QInputDialog&>(*m_dialog);
        dialog.setWindowTitle("Ladybird");
        dialog.setLabelText(qstring_from_ak_string(message));
        dialog.setTextValue(qstring_from_ak_string(default_));

        QObject::connect(m_dialog, &QDialog::finished, this, [this](auto result) {
            if (result == QDialog::Accepted) {
                auto& dialog = static_cast<QInputDialog&>(*m_dialog);
                view().prompt_closed(ak_string_from_qstring(dialog.textValue()));
            } else {
                view().prompt_closed({});
            }

            m_dialog = nullptr;
        });

        m_dialog->open();
    };

    view().on_request_set_prompt_text = [this](auto const& message) {
        if (m_dialog && is<QInputDialog>(*m_dialog))
            static_cast<QInputDialog&>(*m_dialog).setTextValue(qstring_from_ak_string(message));
    };

    view().on_request_accept_dialog = [this]() {
        if (m_dialog)
            m_dialog->accept();
    };

    view().on_request_dismiss_dialog = [this]() {
        if (m_dialog)
            m_dialog->reject();
    };

    view().on_request_color_picker = [this](Color current_color) {
        m_dialog = new QColorDialog(QColor(current_color.red(), current_color.green(), current_color.blue()), &view());

        auto& dialog = static_cast<QColorDialog&>(*m_dialog);
        dialog.setWindowTitle("Ladybird");
        dialog.setOption(QColorDialog::ShowAlphaChannel, false);
        QObject::connect(&dialog, &QColorDialog::currentColorChanged, this, [this](QColor const& color) {
            view().color_picker_update(Color(color.red(), color.green(), color.blue()), Web::HTML::ColorPickerUpdateState::Update);
        });

        QObject::connect(m_dialog, &QDialog::finished, this, [this](auto result) {
            if (result == QDialog::Accepted) {
                auto& dialog = static_cast<QColorDialog&>(*m_dialog);
                view().color_picker_update(Color(dialog.selectedColor().red(), dialog.selectedColor().green(), dialog.selectedColor().blue()), Web::HTML::ColorPickerUpdateState::Closed);
            } else {
                view().color_picker_update({}, Web::HTML::ColorPickerUpdateState::Closed);
            }

            m_dialog = nullptr;
        });

        m_dialog->open();
    };

    view().on_request_file_picker = [this](auto const& accepted_file_types, auto allow_multiple_files) {
        Vector<Web::HTML::SelectedFile> selected_files;

        auto create_selected_file = [&](auto const& qfile_path) {
            auto file_path = ak_byte_string_from_qstring(qfile_path);

            if (auto file = WebView::create_selected_file(file_path); file.is_error())
                warnln("Unable to open file {}: {}", file_path, file.error());
            else
                selected_files.append(file.release_value());
        };

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
                    if (auto mime_type = mime_database.mimeTypeForName(qstring_from_ak_string(filter.value)); mime_type.isValid())
                        accepted_file_filters.append(mime_type.filterString());
                },
                [&](Web::HTML::FileFilter::Extension const& filter) {
                    accepted_file_filters.append(qformatted("*.{}", filter.value));
                });
        }

        accepted_file_filters.size() > 1 ? accepted_file_filters.prepend("All files (*)") : accepted_file_filters.append("All files (*)");
        auto filters = accepted_file_filters.join(";;");

        if (allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes) {
            auto paths = QFileDialog::getOpenFileNames(this, "Select files", QDir::homePath(), filters);
            selected_files.ensure_capacity(static_cast<size_t>(paths.size()));

            for (auto const& path : paths)
                create_selected_file(path);
        } else {
            auto path = QFileDialog::getOpenFileName(this, "Select file", QDir::homePath(), filters);
            create_selected_file(path);
        }

        view().file_picker_closed(std::move(selected_files));
    };

    view().on_find_in_page = [this](auto current_match_index, auto const& total_match_count) {
        m_find_in_page->update_result_label(current_match_index, total_match_count);
    };

    QObject::connect(focus_location_editor_action, &QAction::triggered, this, &Tab::focus_location_editor);

    view().on_restore_window = [this]() {
        m_window->showNormal();
    };

    view().on_reposition_window = [this](auto const& position) {
        m_window->move(position.x(), position.y());
        view().did_update_window_rect();
    };

    view().on_resize_window = [this](auto const& size) {
        m_window->resize(size.width(), size.height());
        view().did_update_window_rect();
    };

    view().on_maximize_window = [this]() {
        m_window->showMaximized();
        view().did_update_window_rect();
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

    view().on_audio_play_state_changed = [this](auto play_state) {
        emit audio_play_state_changed(tab_index(), play_state);
    };

    auto* duplicate_tab_action = new QAction("&Duplicate Tab", this);
    QObject::connect(duplicate_tab_action, &QAction::triggered, this, [this]() {
        m_window->new_tab_from_url(view().url(), Web::HTML::ActivateTab::Yes);
    });

    auto* move_to_start_action = new QAction("Move to &Start", this);
    QObject::connect(move_to_start_action, &QAction::triggered, this, [this]() {
        m_window->move_tab(tab_index(), 0);
    });

    auto* move_to_end_action = new QAction("Move to &End", this);
    QObject::connect(move_to_end_action, &QAction::triggered, this, [this]() {
        m_window->move_tab(tab_index(), m_window->tab_count() - 1);
    });

    auto* close_tab_action = new QAction("&Close Tab", this);
    QObject::connect(close_tab_action, &QAction::triggered, this, [this]() {
        request_close();
    });

    auto* close_tabs_to_left_action = new QAction("C&lose Tabs to Left", this);
    QObject::connect(close_tabs_to_left_action, &QAction::triggered, this, [this]() {
        for (auto i = tab_index() - 1; i >= 0; i--) {
            m_window->request_to_close_tab(i);
        }
    });

    auto* close_tabs_to_right_action = new QAction("Close Tabs to R&ight", this);
    QObject::connect(close_tabs_to_right_action, &QAction::triggered, this, [this]() {
        for (auto i = m_window->tab_count() - 1; i > tab_index(); i--) {
            m_window->request_to_close_tab(i);
        }
    });

    auto* close_other_tabs_action = new QAction("Cl&ose Other Tabs", this);
    QObject::connect(close_other_tabs_action, &QAction::triggered, this, [this]() {
        for (auto i = m_window->tab_count() - 1; i >= 0; i--) {
            if (i == tab_index())
                continue;

            m_window->request_to_close_tab(i);
        }
    });

    m_context_menu = new QMenu("Context menu", this);
    m_context_menu->addAction(create_application_action(*this, application.reload_action(), IncludeActionIcon::No));
    m_context_menu->addAction(duplicate_tab_action);
    m_context_menu->addSeparator();
    auto* move_tab_menu = m_context_menu->addMenu("Mo&ve Tab");
    move_tab_menu->addAction(move_to_start_action);
    move_tab_menu->addAction(move_to_end_action);
    m_context_menu->addSeparator();
    m_context_menu->addAction(close_tab_action);
    auto* close_multiple_tabs_menu = m_context_menu->addMenu("Close &Multiple Tabs");
    close_multiple_tabs_menu->addAction(close_tabs_to_left_action);
    close_multiple_tabs_menu->addAction(close_tabs_to_right_action);
    close_multiple_tabs_menu->addAction(close_other_tabs_action);
}

Tab::~Tab() = default;

void Tab::focus_location_editor()
{
#if defined(AK_OS_MACOS)
    make_appkit_window_first_responder(*m_location_edit);
#endif
    m_location_edit->setFocus();
    m_location_edit->selectAll();
    m_location_edit->show_autocomplete();
}

void Tab::set_window(BrowserWindow& window)
{
    if (&window == m_window)
        return;

    QObject::disconnect(&m_window->hamburger_menu(), nullptr, m_hamburger_button, nullptr);

    m_window = &window;
    m_hamburger_button->setMenu(&m_window->hamburger_menu());
    connect_hamburger_menu();
    recreate_toolbar_icons();
}

void Tab::set_vertical_tabs_enabled(bool enabled)
{
    m_vertical_tabs_enabled = enabled;
    m_toolbar->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    if (m_sidebar_toggle_navigation_spacer)
        m_sidebar_toggle_navigation_spacer->changeSize(enabled ? TOOLBAR_SIDEBAR_TOGGLE_NAVIGATION_GAP : 0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    update_vertical_tabs_toolbar_button_placement();
    m_toolbar->layout()->invalidate();
}

void Tab::set_vertical_tabs_position(WebView::VerticalTabsPosition position)
{
    if (m_vertical_tabs_position == position)
        return;

    m_vertical_tabs_position = position;
    recreate_toolbar_icons();
    update_vertical_tabs_toolbar_button_placement();
    m_toolbar->layout()->invalidate();
}

void Tab::set_toolbar_container_in_tab_layout(bool in_tab_layout)
{
    auto* tab_layout = static_cast<QBoxLayout*>(layout());
    auto index = tab_layout->indexOf(m_toolbar_container);

    if (in_tab_layout) {
        if (index == -1) {
            m_toolbar_container->setParent(this);
            tab_layout->insertWidget(0, m_toolbar_container);
        }
        m_toolbar_container->show();
        return;
    }

    if (index != -1) {
        tab_layout->removeWidget(m_toolbar_container);
        m_toolbar_container->hide();
    }
}

void Tab::set_toolbar_window_controls_visible(bool visible)
{
    auto const has_left_traffic_lights = use_left_traffic_light_window_controls() && visible;
    auto const has_trailing_window_controls = use_right_custom_window_controls() && visible;

    m_toolbar_window_controls_separator->setVisible(has_trailing_window_controls);
    m_toolbar_window_controls->setVisible(has_left_traffic_lights || has_trailing_window_controls);
    if (m_toolbar_window_controls_spacer)
        m_toolbar_window_controls_spacer->changeSize(has_left_traffic_lights ? TOOLBAR_MACOS_TRAFFIC_LIGHTS_CONTROL_GAP : 0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    m_toolbar->layout()->setContentsMargins(TOOLBAR_HORIZONTAL_MARGIN, TOOLBAR_VERTICAL_MARGIN, has_trailing_window_controls ? TOOLBAR_WINDOW_CONTROLS_RIGHT_MARGIN : TOOLBAR_HORIZONTAL_MARGIN, TOOLBAR_VERTICAL_MARGIN);
    m_toolbar->layout()->invalidate();
}

void Tab::update_window_control_icons()
{
    auto is_maximized = m_window->isMaximized();
    m_minimize_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowMinimize, palette()));
    m_maximize_window_button->setIcon(create_chrome_icon(is_maximized ? ChromeIcon::WindowRestore : ChromeIcon::WindowMaximize, palette()));
    m_maximize_window_button->setToolTip(is_maximized ? "Restore" : "Maximize");
    m_close_window_button->setIcon(create_chrome_icon(ChromeIcon::WindowClose, palette()));
}

void Tab::connect_hamburger_menu()
{
    QObject::connect(&m_window->hamburger_menu(), &QMenu::aboutToShow, m_hamburger_button, [this]() {
        m_hamburger_button->setDown(true);
    });
    QObject::connect(&m_window->hamburger_menu(), &QMenu::aboutToHide, m_hamburger_button, [this]() {
        m_hamburger_button->setDown(false);
    });

    update_hamburger_menu();
}

void Tab::update_hamburger_menu()
{
    auto show_menu_bar = show_menubar_option_available() && WebView::Application::settings().show_menu_bar();
    m_hamburger_button->setVisible(!show_menu_bar);
}

void Tab::navigate(URL::URL const& url)
{
    view().load(url);
}

void Tab::load_html(StringView html)
{
    view().load_html(html);
}

void Tab::location_edit_return_pressed()
{
    auto text = m_location_edit->text();
    if (text.isEmpty())
        return;

    if (auto url = m_location_edit->url(); url.has_value())
        navigate(*url);
    else
        view().load_navigation_error_page(ak_string_from_qstring(text));

    view().setFocus();
}

QIcon Tab::tab_icon() const
{
    if (m_is_loading)
        return loading_spinner_icon(palette(), m_loading_animation_frame);
    return m_favicon;
}

QString Tab::title() const
{
    if (!WebView::Application::settings().config_variable_as_bool(WebView::ConfigVariableID::ShowWebContentProcessIDInTabTitle))
        return m_title;

    return QString("%1 [%2]").arg(m_title).arg(view().client().pid());
}

void Tab::update_tab_title()
{
    emit title_changed(tab_index(), title());
}

void Tab::show_menu_bar_changed()
{
    update_hamburger_menu();
}

void Tab::tab_settings_changed()
{
    auto const& tab_settings = WebView::Application::settings().tab_settings();
    m_vertical_tabs_enabled = tab_settings.vertical_tabs_enabled;
    m_vertical_tabs_position = tab_settings.vertical_tabs_position;
    if (m_sidebar_toggle_navigation_spacer)
        m_sidebar_toggle_navigation_spacer->changeSize(m_vertical_tabs_enabled ? TOOLBAR_SIDEBAR_TOGGLE_NAVIGATION_GAP : 0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
    recreate_toolbar_icons();
    update_vertical_tabs_toolbar_button_placement();
    m_toolbar->layout()->invalidate();
}

void Tab::config_variable_changed(WebView::ConfigVariableID variable)
{
    if (variable == WebView::ConfigVariableID::ShowWebContentProcessIDInTabTitle)
        update_tab_title();
}

void Tab::set_loading(bool is_loading)
{
    if (m_is_loading == is_loading)
        return;

    m_is_loading = is_loading;
    m_loading_animation_frame = 0;

    if (m_is_loading)
        m_loading_animation_timer->start();
    else
        m_loading_animation_timer->stop();

    update_tab_icon();
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

void Tab::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_hover_label->isVisible())
        update_hover_label();
}

void Tab::update_hover_label()
{
    m_hover_label->setText(QFontMetrics(m_hover_label->font()).elidedText(m_hover_label->text(), Qt::ElideRight, width() / 2 - 10));
    m_hover_label->resize(m_hover_label->sizeHint());

    auto hover_label_height = height() - m_hover_label->height();
    if (m_find_in_page->isVisible())
        hover_label_height -= m_find_in_page->height();

    if (m_hover_label->underMouse() && m_hover_label->x() == 0)
        m_hover_label->move(width() / 2 + (width() / 2 - m_hover_label->width()), hover_label_height);
    else
        m_hover_label->move(0, hover_label_height);

    m_hover_label->raise();
}

bool Tab::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange) {
        update_chrome_style();
        recreate_toolbar_icons();
        if (m_is_loading)
            update_tab_icon();
        return QWidget::event(event);
    }

    return QWidget::event(event);
}

void Tab::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;
    m_toolbar_container->setStyleSheet(ChromeStyle::toolbar_container_style_sheet(palette()));
    auto hover_surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface(palette()));
    auto hover_border = ChromeStyle::style_sheet_color(ChromeStyle::chrome_border(palette()));
    auto hover_text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_text(palette()));
    m_hover_label->setStyleSheet(qformatted("background: {}; color: {}; border: 1px solid {}; border-radius: 6px;",
        hover_surface, hover_text, hover_border));
    if (m_downloads_popover)
        m_downloads_popover->update_chrome_style(palette());
    m_is_updating_chrome_style = false;
}

void Tab::recreate_toolbar_icons()
{
    auto vertical_tabs_are_expanded = WebView::Application::settings().tab_settings().vertical_tabs_expanded;
    auto vertical_tabs_icon = m_vertical_tabs_position == WebView::VerticalTabsPosition::Right
        ? (vertical_tabs_are_expanded ? ChromeIcon::VerticalTabBarCollapseRight : ChromeIcon::VerticalTabBarExpandRight)
        : (vertical_tabs_are_expanded ? ChromeIcon::VerticalTabBarCollapse : ChromeIcon::VerticalTabBarExpand);
    m_toggle_vertical_tabs_expanded_action->setIcon(create_chrome_icon(vertical_tabs_icon, palette()));
    m_navigate_back_action->setIcon(create_chrome_icon(ChromeIcon::Back, palette()));
    m_navigate_forward_action->setIcon(create_chrome_icon(ChromeIcon::Forward, palette()));
    m_reload_action->setIcon(create_chrome_icon(ChromeIcon::Reload, palette()));
    m_downloads_button_icon = {};
    update_downloads_button();
    m_hamburger_button->setIcon(create_chrome_icon(ChromeIcon::Menu, palette()));
    update_window_control_icons();

    if (auto* action = m_location_edit->trailing_action()) {
        auto icon = view().toggle_bookmark_action().engaged() ? ChromeIcon::StarFilled : ChromeIcon::Star;
        action->setIcon(create_chrome_icon(icon, palette()));
    }
}

void Tab::update_downloads_button()
{
    if (!m_downloads_button)
        return;

    auto downloads = WebView::Application::the().file_downloader().downloads();
    using DownloadStatus = WebView::FileDownloader::DownloadStatus;

    size_t active_download_count = 0;
    size_t unknown_active_download_count = 0;
    size_t failed_download_count = 0;
    double known_downloaded_size = 0.0;
    double known_total_size = 0.0;
    for (auto const& download : downloads) {
        switch (download.status) {
        case DownloadStatus::InProgress:
            ++active_download_count;
            if (download.total_size.has_value() && *download.total_size > 0) {
                known_downloaded_size += min(download.downloaded_size, *download.total_size);
                known_total_size += *download.total_size;
            } else {
                ++unknown_active_download_count;
            }
            break;
        case DownloadStatus::Completed:
            break;
        case DownloadStatus::Canceled:
            break;
        case DownloadStatus::Failed:
            ++failed_download_count;
            break;
        }
    }

    m_downloads_button->setVisible(!downloads.is_empty());
    if (downloads.is_empty()) {
        static_cast<DownloadsButton*>(m_downloads_button)->set_progress({});
        if (m_downloads_popover)
            m_downloads_popover->close();
    }

    Optional<double> active_download_progress;
    if (known_total_size > 0.0)
        active_download_progress = known_downloaded_size / known_total_size;

    auto downloads_icon = active_download_count > 0 && !active_download_progress.has_value() ? ChromeIcon::DownloadActive : ChromeIcon::Download;
    if (!m_downloads_button_icon.has_value() || *m_downloads_button_icon != downloads_icon) {
        auto icon = create_chrome_icon(downloads_icon, palette());
        m_open_downloads_page_action->setIcon(icon);
        m_downloads_button->setIcon(icon);
        static_cast<DownloadsButton*>(m_downloads_button)->set_progress_icon(create_chrome_icon(ChromeIcon::DownloadActive, palette()));
        m_downloads_button_icon = downloads_icon;
    }

    static_cast<DownloadsButton*>(m_downloads_button)->set_progress(active_download_count > 0 ? active_download_progress : Optional<double> {});

    QString tooltip;
    if (active_download_count > 0) {
        if (active_download_progress.has_value() && unknown_active_download_count == 0) {
            if (active_download_count == 1) {
                tooltip = QString("Downloading - %1")
                              .arg(download_percent_text(*active_download_progress));
            } else {
                tooltip = QString("%1 downloads - %2")
                              .arg(active_download_count)
                              .arg(download_percent_text(*active_download_progress));
            }
        } else {
            tooltip = download_count_text(active_download_count, "download in progress", "downloads in progress");
        }
    } else if (failed_download_count > 0) {
        tooltip = download_count_text(failed_download_count, "download failed", "downloads failed");
    } else {
        tooltip = "Downloads";
    }

    if (m_downloads_button_tooltip != tooltip) {
        m_downloads_button_tooltip = tooltip;
        m_open_downloads_page_action->setToolTip(tooltip);
        m_downloads_button->setToolTip(tooltip);
    }
}

void Tab::update_downloads_popover()
{
    if (!m_downloads_popover || !m_downloads_popover->isVisible())
        return;

    if (m_downloads_popover->set_downloads(WebView::Application::the().file_downloader().downloads()))
        position_downloads_popover();
}

void Tab::show_downloads_popover()
{
    if (!m_downloads_button || !m_downloads_button->isVisible())
        return;

    if (!m_downloads_popover) {
        m_downloads_popover = new DownloadsPopover(this);
        m_downloads_popover->on_cancel_download = [this](u64 id) {
            auto& file_downloader = WebView::Application::the().file_downloader();
            if (auto download = file_downloader.download(id); download.has_value() && download->status == WebView::FileDownloader::DownloadStatus::InProgress)
                file_downloader.cancel_download(id);
            update_downloads_popover();
        };
        m_downloads_popover->on_open_all_downloads = [this] {
            if (m_downloads_popover)
                m_downloads_popover->close();
            m_open_downloads_page_action->trigger();
        };
    }

    m_downloads_popover->update_chrome_style(palette());
    (void)m_downloads_popover->set_downloads(WebView::Application::the().file_downloader().downloads());
    position_downloads_popover();
    m_downloads_popover->show();
    position_downloads_popover();
    m_downloads_popover->raise();
}

void Tab::position_downloads_popover()
{
    if (!m_downloads_popover || !m_downloads_button)
        return;

    m_downloads_popover->adjustSize();
    auto size = m_downloads_popover->sizeHint();
    size.setWidth(DOWNLOADS_POPOVER_WIDTH);
    if (size.height() > DOWNLOADS_POPOVER_MAX_HEIGHT)
        size.setHeight(DOWNLOADS_POPOVER_MAX_HEIGHT);
    m_downloads_popover->setFixedSize(size);

    auto anchor_position = m_downloads_button->mapToGlobal(m_downloads_button->rect().bottomRight());
    auto popup_position = QPoint(anchor_position.x() - m_downloads_popover->width(), anchor_position.y() + 4);

    if (auto* screen = QGuiApplication::screenAt(anchor_position)) {
        auto available_geometry = screen->availableGeometry();
        if (popup_position.x() < available_geometry.left())
            popup_position.setX(available_geometry.left());
        if (popup_position.x() + m_downloads_popover->width() > available_geometry.right())
            popup_position.setX(available_geometry.right() - m_downloads_popover->width() + 1);
        if (popup_position.y() + m_downloads_popover->height() > available_geometry.bottom())
            popup_position.setY(m_downloads_button->mapToGlobal(m_downloads_button->rect().topRight()).y() - m_downloads_popover->height() - 4);
    }

    m_downloads_popover->move(popup_position);
}

void Tab::download_added(WebView::FileDownloader::Download const&)
{
    update_downloads_button();
    if (Application::the().active_tab() == this && m_window->isActiveWindow()) {
        show_downloads_popover();
        return;
    }
    update_downloads_popover();
}

void Tab::download_updated(WebView::FileDownloader::Download const&)
{
    update_downloads_button();
    update_downloads_popover();
}

void Tab::download_removed(u64)
{
    update_downloads_button();
    update_downloads_popover();
}

void Tab::update_vertical_tabs_toolbar_button_placement()
{
    auto show_left_button = m_vertical_tabs_enabled && m_vertical_tabs_position == WebView::VerticalTabsPosition::Left;
    auto show_right_button = m_vertical_tabs_enabled && m_vertical_tabs_position == WebView::VerticalTabsPosition::Right;

    if (m_left_toggle_vertical_tabs_expanded_button)
        m_left_toggle_vertical_tabs_expanded_button->setVisible(show_left_button);
    if (m_right_toggle_vertical_tabs_expanded_button)
        m_right_toggle_vertical_tabs_expanded_button->setVisible(show_right_button);
}

void Tab::show_find_in_page()
{
    m_find_in_page->setVisible(true);
    m_find_in_page->setFocus();
}

void Tab::find_previous()
{
    m_find_in_page->find_previous();
}

void Tab::find_next()
{
    m_find_in_page->find_next();
}

void Tab::request_close()
{
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

}
