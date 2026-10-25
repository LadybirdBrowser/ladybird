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
#include <LibURL/URL.h>
#include <LibWakeLock/DisplaySleepInhibitor.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebView/Application.h>
#include <LibWebView/DownloadPresentation.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/ContentBlockingButton.h>
#include <UI/Qt/ContentBlockingPopover.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/JavaScriptDialog.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif
#include <UI/Qt/Menu.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/WindowControlButton.h>

#include <LibWebView/TabPerformanceMonitor.h>
#include <QApplication>
#include <QColorDialog>
#include <QCursor>
#include <QDynamicPropertyChangeEvent>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <UI/Qt/PerformanceMonitorWidget.h>

namespace Ladybird {

static constexpr auto WINDOW_DRAG_REGION_PROPERTY = "LadybirdWindowDragRegion";
static constexpr int BOOKMARKS_BAR_HOVER_COLLAPSE_POLL_INTERVAL_MS = 250;

class ToolbarButton : public QToolButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    using QToolButton::QToolButton;

    Function<void()> on_middle_click;

protected:
    virtual void paintEvent(QPaintEvent*) override
    {
        QStyleOptionToolButton option;
        initStyleOption(&option);

        QStylePainter painter(this);
        ChromeStyle::paint_circular_control_frame(painter, *this);

        painter.drawComplexControl(QStyle::CC_ToolButton, option);
    }

    virtual void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::MiddleButton && on_middle_click)
            event->accept();
        else
            QToolButton::mousePressEvent(event);
    }

    virtual void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::MiddleButton && on_middle_click) {
            if (rect().contains(event->position().toPoint()))
                on_middle_click();
            event->accept();
        } else {
            QToolButton::mouseReleaseEvent(event);
        }
    }
};

// The reload button becomes a stop button while the page loads. So that one click does not do what the user did not aim
// at, the button does not change at the wrong moment while under the pointer: a stop button stays until the pointer
// leaves or a short delay passes, and a reload click does not become a stop button until the double-click interval
// passes, so a double-click does not stop the reload it started. The reload action stays attached to the button while
// it shows stop, so its shortcuts still work during a load.
class ReloadButton final : public ToolbarButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ReloadButton(QWidget* parent)
        : ToolbarButton(parent)
    {
        m_double_click_timer.setSingleShot(true);
        m_double_click_timer.setInterval(QApplication::doubleClickInterval());
        QObject::connect(&m_double_click_timer, &QTimer::timeout, this, [this] {
            change_mode(m_intended_mode, ForceChange::No);
        });

        m_stop_to_reload_timer.setSingleShot(true);
        m_stop_to_reload_timer.setInterval(STOP_TO_RELOAD_DELAY_MS);
        QObject::connect(&m_stop_to_reload_timer, &QTimer::timeout, this, [this] {
            change_mode(m_intended_mode, ForceChange::Yes);
        });

        QObject::connect(this, &QToolButton::pressed, this, [this] {
            if (m_visible_mode == Mode::Reload)
                m_double_click_timer.start();
        });

        // NB: A stop click that arrives after the load ended has nothing to stop, so show the reload button right away.
        QObject::connect(this, &QToolButton::clicked, this, [this] {
            if (m_visible_mode == Mode::Stop)
                change_mode(m_intended_mode, ForceChange::Yes);
        });
    }

    void set_actions(QAction& reload_action, QAction& reload_bypassing_cache_action, QAction& stop_action)
    {
        m_reload_action = &reload_action;
        m_reload_bypassing_cache_action = &reload_bypassing_cache_action;
        m_stop_action = &stop_action;
    }

    void set_loading(bool is_loading)
    {
        change_mode(is_loading ? Mode::Stop : Mode::Reload, ForceChange::No);
    }

protected:
    virtual void leaveEvent(QEvent* event) override
    {
        ToolbarButton::leaveEvent(event);
        change_mode(m_intended_mode, ForceChange::Yes);
    }

    // A Shift-click on the reload button reloads bypassing the HTTP cache, and one on the stop button stops the load as
    // any other click there does. QToolButton triggers its default action from here, once a click has completed.
    virtual void nextCheckState() override
    {
        if (m_visible_mode == Mode::Reload && QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)) {
            m_reload_bypassing_cache_action->trigger();
            return;
        }
        ToolbarButton::nextCheckState();
    }

private:
    static constexpr int STOP_TO_RELOAD_DELAY_MS = 1000;

    enum class Mode {
        Reload,
        Stop,
    };

    enum class ForceChange {
        No,
        Yes,
    };

    void change_mode(Mode mode, ForceChange force_change)
    {
        m_intended_mode = mode;

        auto change_is_safe = mode == Mode::Stop ? !m_double_click_timer.isActive() : m_visible_mode != Mode::Stop;
        if (force_change == ForceChange::Yes || !underMouse() || change_is_safe) {
            m_double_click_timer.stop();
            m_stop_to_reload_timer.stop();
            m_visible_mode = mode;
            setDefaultAction(mode == Mode::Stop ? m_stop_action : m_reload_action);
            return;
        }

        if (m_visible_mode == Mode::Stop && !m_stop_to_reload_timer.isActive())
            m_stop_to_reload_timer.start();
    }

    QAction* m_reload_action { nullptr };
    QAction* m_reload_bypassing_cache_action { nullptr };
    QAction* m_stop_action { nullptr };
    Mode m_visible_mode { Mode::Reload };
    Mode m_intended_mode { Mode::Reload };
    QTimer m_double_click_timer;
    QTimer m_stop_to_reload_timer;
};

class HamburgerButton final : public ToolbarButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    using ToolbarButton::ToolbarButton;

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

class DownloadsButton final : public ToolbarButton {
public:
    AK_ALLOC_WITH_KMALLOC;

    using ToolbarButton::ToolbarButton;

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
        ToolbarButton::paintEvent(event);

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

static constexpr int TOOLBAR_BUTTON_SIZE = 34;

template<typename ButtonType = ToolbarButton>
static ButtonType* create_toolbar_button(QWidget& parent, QAction& action)
{
    auto* button = new ButtonType(&parent);
    button->setDefaultAction(&action);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize({ 20, 20 });
    button->setFixedSize(TOOLBAR_BUTTON_SIZE, TOOLBAR_BUTTON_SIZE);

    // FIXME: In Menu.cpp, we set the initial visibility of the action before we've associated it with this QToolButton.
    //        It would be nicer if we didn't have to do this here.
    button->setVisible(action.isVisible());

    return button;
}

QToolButton* Tab::create_navigation_history_toolbar_button(QWidget& parent, QAction& action, int direction)
{
    auto& view = this->view();
    auto* button = create_toolbar_button(parent, action);
    button->on_middle_click = [this, direction] {
        m_window->open_history_entry_in_new_tab(*this, direction);
    };
    auto* menu = new QMenu(button);
    QObject::connect(menu, &QMenu::aboutToShow, button, [menu, &view, direction] {
        populate_session_history_traversal_menu(*menu, view, direction);
    });
    button->setMenu(menu);
    button->setPopupMode(QToolButton::DelayedPopup);
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(button, &QToolButton::customContextMenuRequested, button, [button, menu, &view, direction](QPoint const&) {
        populate_session_history_traversal_menu(*menu, view, direction);
        if (!menu->isEmpty())
            button->showMenu();
    });
    return button;
}

static constexpr int TAB_FAVICON_SIZE = 16;
static constexpr int TOOLBAR_HORIZONTAL_MARGIN = 12;
#ifdef AK_OS_MACOS
static constexpr int WEB_VIEW_INSET = 5;
// The sidebar has its own padding, so a narrower frame of the toolbar's color sets the page apart from it.
static constexpr int WEB_VIEW_SIDEBAR_INSET = 4;
#endif
static constexpr int TOOLBAR_VERTICAL_MARGIN = 2;
// NB: The location field gets a point more room below it than above it, where the page begins.
static constexpr int TOOLBAR_BOTTOM_MARGIN = TOOLBAR_VERTICAL_MARGIN + 1;
static constexpr int TOOLBAR_WINDOW_CONTROLS_RIGHT_MARGIN = 4;
static constexpr int DOWNLOADS_POPOVER_WIDTH = 380;
static constexpr int DOWNLOADS_POPOVER_MAX_HEIGHT = 360;
static constexpr int PRIVATE_SESSION_POPOVER_WIDTH = 320;

class ElidedLabel final : public QLabel {
public:
    AK_ALLOC_WITH_KMALLOC;

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
    AK_ALLOC_WITH_KMALLOC;

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

        m_pause_button = new QPushButton("Pause", this);
        m_pause_button->setFocusPolicy(Qt::NoFocus);
        QObject::connect(m_pause_button, &QPushButton::clicked, this, [this] {
            if (m_is_paused) {
                if (on_resume_download)
                    on_resume_download(m_download_id);
            } else if (on_pause_download) {
                on_pause_download(m_download_id);
            }
        });
        row_layout->addWidget(m_pause_button, 0, Qt::AlignTop);

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

        auto status_text = qstring_from_ak_string(WebView::download_status_text(download));
        m_status_label->setText(status_text);
        m_status_label->setToolTip(status_text);

        bool geometry_changed = false;
        auto progress = download.progress();
        auto show_progress = WebView::FileDownloader::status_is_active(download.status) && progress.has_value();
        auto progress_bar_is_visible = !m_progress_bar->isHidden();
        if (progress_bar_is_visible != show_progress) {
            m_progress_bar->setVisible(show_progress);
            geometry_changed = true;
        }
        if (progress.has_value())
            m_progress_bar->setValue(static_cast<int>(*progress * 1000.0));

        m_is_paused = download.status == WebView::FileDownloader::DownloadStatus::Paused;
        m_pause_button->setText(m_is_paused ? "Resume" : "Pause");

        auto should_show_pause = download.can_resume && WebView::FileDownloader::status_is_active(download.status);
        auto pause_button_is_visible = !m_pause_button->isHidden();
        if (pause_button_is_visible != should_show_pause) {
            m_pause_button->setVisible(should_show_pause);
            geometry_changed = true;
        }

        auto should_show_cancel = WebView::FileDownloader::status_is_active(download.status);
        auto cancel_button_is_visible = !m_cancel_button->isHidden();
        if (cancel_button_is_visible != should_show_cancel) {
            m_cancel_button->setVisible(should_show_cancel);
            geometry_changed = true;
        }

        return geometry_changed;
    }

    Function<void(u64)> on_cancel_download;
    Function<void(u64)> on_pause_download;
    Function<void(u64)> on_resume_download;

private:
    u64 m_download_id { 0 };
    bool m_is_paused { false };
    ElidedLabel* m_file_name_label { nullptr };
    ElidedLabel* m_status_label { nullptr };
    QProgressBar* m_progress_bar { nullptr };
    QPushButton* m_pause_button { nullptr };
    QPushButton* m_cancel_button { nullptr };
};

class DownloadsPopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit DownloadsPopover(QWidget* parent)
        : Popover(parent)
    {
        card().setObjectName("LadybirdDownloadsPopover");
        card().setFixedWidth(DOWNLOADS_POPOVER_WIDTH);

        auto* layout = new QVBoxLayout(&card());
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
        setStyleSheet(ChromeStyle::downloads_popover_style_sheet(palette));
    }

    bool set_downloads(ReadonlySpan<WebView::FileDownloader::Download> downloads)
    {
        bool geometry_changed = false;
        auto visible_downloads = WebView::recent_downloads_for_popover(downloads);

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
    Function<void(u64)> on_pause_download;
    Function<void(u64)> on_resume_download;
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
            row->on_pause_download = [this](u64 id) {
                if (on_pause_download)
                    on_pause_download(id);
            };
            row->on_resume_download = [this](u64 id) {
                if (on_resume_download)
                    on_resume_download(id);
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

    // TabWidget keeps every tab's toolbar container in a stack beside its pages, so even a tab that hasn't loaded needs
    // one. initialize() fills it.
    m_toolbar_container = new QWidget(this);
    m_toolbar_container->setObjectName("LadybirdToolbarContainer");
    m_toolbar_container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    create_tab_context_menu();
}

void Tab::initialize(Optional<WebView::CanonicalTraversable&> traversable)
{
    auto& application = WebView::Application::the();
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
    m_find_in_page = new FindInPageWidget(this, m_view);
    m_find_in_page->setVisible(false);

    m_toolbar = new QWidget(this);
    m_toolbar->setObjectName("LadybirdNavigationToolbar");
    m_toolbar->setFixedHeight(browser_chrome_layout_policy().toolbar_height);
    m_toolbar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);

    auto* toolbar_container_layout = new QVBoxLayout(m_toolbar_container);
    toolbar_container_layout->setSpacing(0);
    toolbar_container_layout->setContentsMargins(0, 0, 0, 0);

    auto* toolbar_layout = new QHBoxLayout(m_toolbar);
    toolbar_layout->setSpacing(6);
    toolbar_layout->setContentsMargins(TOOLBAR_HORIZONTAL_MARGIN, TOOLBAR_VERTICAL_MARGIN, TOOLBAR_HORIZONTAL_MARGIN, TOOLBAR_BOTTOM_MARGIN);

    m_location_edit = new LocationEdit(this, m_window->is_private());
    m_loading_animation_timer = new QTimer(this);
    m_loading_animation_timer->setInterval(80);

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

    m_toolbar->installEventFilter(this);
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
    QObject::connect(m_loading_animation_timer, &QTimer::timeout, this, [this] {
        m_loading_animation_frame = (m_loading_animation_frame + 1) % 12;
        update_tab_icon();
    });

    auto* focus_location_editor_action = new QAction("Edit Location", this);
    focus_location_editor_action->setShortcuts({ QKeySequence("Ctrl+L"), QKeySequence("Alt+D") });
    addAction(focus_location_editor_action);

    toolbar_container_layout->addWidget(m_toolbar);
    toolbar_container_layout->addWidget(m_bookmarks_bar_container);
#ifdef AK_OS_MACOS
    m_view_layout = new QHBoxLayout;
    m_view_layout->addWidget(m_view);
    update_web_view_frame(m_web_view_chrome_visible, m_web_view_sidebar_position);
    tab_layout->addLayout(m_view_layout);
#else
    tab_layout->addWidget(m_view);
#endif
    tab_layout->addWidget(m_find_in_page);
    update_bookmarks_bar_visibility();

    m_navigate_back_action = create_application_action(*this, view().navigate_back_action());
    m_navigate_forward_action = create_application_action(*this, view().navigate_forward_action());
    m_reload_action = create_application_action(*this, application.reload_action());
    m_reload_bypassing_cache_action = create_application_action(*this, application.reload_bypassing_cache_action());
    addAction(m_reload_bypassing_cache_action);
    m_stop_action = new QAction("Stop", this);
    m_stop_action->setToolTip("Stop loading this page");
    QObject::connect(m_stop_action, &QAction::triggered, this, [this] {
        view().stop_loading();
    });
    m_open_downloads_page_action = create_application_action(*this, application.open_downloads_page_action());

    m_downloads_button = new DownloadsButton(m_toolbar);
    m_downloads_button->setText("Downloads");
    m_downloads_button->setAutoRaise(true);
    m_downloads_button->setFocusPolicy(Qt::NoFocus);
    m_downloads_button->setIconSize({ 20, 20 });
    m_downloads_button->setFixedSize(TOOLBAR_BUTTON_SIZE, TOOLBAR_BUTTON_SIZE);
    m_downloads_button->setVisible(m_open_downloads_page_action->isVisible());
    QObject::connect(m_downloads_button, &QToolButton::clicked, this, [this] {
        show_downloads_popover();
    });

    m_private_badge = new QPushButton("Private", m_toolbar);
    m_private_badge->setObjectName("LadybirdPrivateBadge");
    m_private_badge->setFixedHeight(22);
    m_private_badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_private_badge->setFocusPolicy(Qt::NoFocus);
    m_private_badge->setToolTip("Close all private windows and start a fresh private browsing session");
    m_private_badge->setVisible(m_window->is_private() == WebView::IsPrivate::Yes);
    QObject::connect(m_private_badge, &QPushButton::clicked, this, [this] {
        show_private_session_popover();
    });

    m_content_blocking_button = new ContentBlockingButton(m_toolbar);
    m_content_blocking_button->setToolTip("Ad Block");
    QObject::connect(m_content_blocking_button, &QToolButton::clicked, this, [this] { show_content_blocking_popover(); });
    view().on_content_blocking_change = [this](auto const& snapshot) { update_content_blocking_controls(snapshot); };
    update_content_blocking_controls(view().content_blocking_snapshot());
    content_blocker_count_visibility_changed();

    m_hamburger_button = new HamburgerButton(m_toolbar);
    m_hamburger_button->setObjectName("LadybirdHamburgerButton");
    m_hamburger_button->setText("Show &Menu");
    m_hamburger_button->setToolTip("Show Menu");
    m_hamburger_button->setIcon(create_chrome_icon(ChromeIcon::Menu, palette()));
    m_hamburger_button->setIconSize({ 20, 20 });
    m_hamburger_button->setFixedSize(TOOLBAR_BUTTON_SIZE, TOOLBAR_BUTTON_SIZE);
    m_hamburger_button->setAutoRaise(true);
    m_hamburger_button->setFocusPolicy(Qt::NoFocus);
    m_hamburger_button->setPopupMode(QToolButton::InstantPopup);
    m_hamburger_button->setMenu(&m_window->hamburger_menu());
    connect_hamburger_menu();

    m_toolbar_window_controls_separator = new QWidget(m_toolbar);
    m_toolbar_window_controls_separator->setObjectName("LadybirdToolbarWindowControlsSeparator");
    m_toolbar_window_controls_separator->setFixedSize(1, 22);

    if constexpr (use_native_macos_window_controls()) {
        auto const native_controls_width = NATIVE_MACOS_WINDOW_CONTROLS_WIDTH + NATIVE_MACOS_WINDOW_CONTROLS_GAP;
        m_toolbar_window_controls = create_window_controls_spacer(*m_toolbar, "LadybirdToolbarWindowControls", { native_controls_width, 38 });
    } else {
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
    }

    recreate_toolbar_icons();

    m_page_context_menu = create_context_menu(*this, view(), view().page_context_menu());
    m_link_context_menu = create_context_menu(*this, view(), view().link_context_menu());
    m_selected_text_link_context_menu = create_context_menu(*this, view(), view().selected_text_link_context_menu());
    m_image_context_menu = create_context_menu(*this, view(), view().image_context_menu());
    m_media_context_menu = create_context_menu(*this, view(), view().media_context_menu());

    auto* navigation_button_cluster = new QWidget(m_toolbar);
    navigation_button_cluster->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    auto* navigation_button_layout = new QHBoxLayout(navigation_button_cluster);
    navigation_button_layout->setSpacing(2);
    navigation_button_layout->setContentsMargins(0, 0, 0, 0);

    navigation_button_layout->addWidget(create_navigation_history_toolbar_button(*navigation_button_cluster, *m_navigate_back_action, -1));
    navigation_button_layout->addWidget(create_navigation_history_toolbar_button(*navigation_button_cluster, *m_navigate_forward_action, 1));
    m_reload_button = create_toolbar_button<ReloadButton>(*navigation_button_cluster, *m_reload_action);
    m_reload_button->set_actions(*m_reload_action, *m_reload_bypassing_cache_action, *m_stop_action);
    navigation_button_layout->addWidget(m_reload_button);

    auto* location_edit_container = new QWidget(m_toolbar);
    location_edit_container->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    auto* location_edit_layout = new QHBoxLayout(location_edit_container);
    location_edit_layout->setSpacing(0);
    location_edit_layout->setContentsMargins(0, 0, 0, 0);

    m_location_edit->set_trailing_action(create_application_action(*m_location_edit, view().toggle_bookmark_action()));
    m_location_edit->set_zoom_action(create_application_action(*m_location_edit, view().reset_zoom_action(), IncludeActionIcon::No));
    location_edit_layout->addWidget(m_location_edit);

    m_performance_monitor = new PerformanceMonitorWidget(m_toolbar);
    (void)WebView::TabPerformanceMonitor::the();
    m_performance_monitor->setVisible(WebView::Application::settings().config_variable_as_bool(WebView::ConfigVariableID::ShowTabPerformanceMonitor));
    view().on_performance_stats = [this](WebView::TabPerformanceStats const& stats) {
        static_cast<PerformanceMonitorWidget*>(m_performance_monitor)->set_stats(stats);
    };

    if constexpr (use_native_macos_window_controls())
        toolbar_layout->addWidget(m_toolbar_window_controls, 0, Qt::AlignVCenter);

    toolbar_layout->addWidget(navigation_button_cluster, 0, Qt::AlignVCenter);
    toolbar_layout->addWidget(location_edit_container, 1);
    toolbar_layout->addWidget(m_performance_monitor, 0, Qt::AlignVCenter);
    toolbar_layout->addWidget(m_downloads_button, 0, Qt::AlignVCenter);
    toolbar_layout->addWidget(m_private_badge, 0, Qt::AlignVCenter);
    toolbar_layout->addWidget(m_content_blocking_button, 0, Qt::AlignVCenter);
    toolbar_layout->addWidget(m_hamburger_button, 0, Qt::AlignVCenter);

    if constexpr (!use_native_macos_window_controls()) {
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
        tab.view().review_web_compat_report(AK::move(capture));
        tab.enter_report_review(this, tr("Report Website Issue"));
    };

    view().on_link_hover = [this](auto const& url) {
        m_hover_label->setText(qstring_from_ak_string(url.to_byte_string()));
        update_hover_label();
        m_hover_label->setCursor(view().cursor());
        m_hover_label->show();
    };

    view().on_link_unhover = [this]() {
        m_hover_label->hide();
    };

    view().on_loading_state_change = [this](bool is_loading) {
        if (!is_loading)
            m_content_blocking_popover_reload_site.clear();
        set_loading(is_loading);
    };
    view().on_load_start = [this] {
        m_window->on_tab_load_start(*this);
        if (m_content_blocking_popover && !m_content_blocking_popover_reload_site.has_value())
            m_content_blocking_popover->close();
    };
    set_loading(view().is_loading());

    view().on_load_finish = [this](auto const&) {
        m_suppress_javascript_dialogs_until_navigation = false;
    };

    view().on_top_level_navigation_commit = [this] {
        if (m_content_blocking_popover) {
            auto snapshot = view().content_blocking_snapshot();
            if (!m_content_blocking_popover_reload_site.has_value() || snapshot.site != m_content_blocking_popover_reload_site) {
                m_content_blocking_popover->close();
            } else {
                m_content_blocking_popover_reload_site.clear();
                update_content_blocking_controls(snapshot);
            }
        }
        m_suppress_javascript_dialogs_until_navigation = false;
    };

    view().on_browser_history_traversal_complete = [this] {
        m_suppress_javascript_dialogs_until_navigation = false;
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
        m_title = qstring_from_utf16_string(title);
        update_tab_title();
    };

    view().on_favicon_change = [this](auto const& bitmap) {
        if (!bitmap.has_value()) {
            m_favicon = {};
            if (m_content_blocking_popover)
                m_content_blocking_popover->set_favicon(m_favicon);
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
        if (m_content_blocking_popover)
            m_content_blocking_popover->set_favicon(m_favicon);
        update_tab_icon();
    };

    auto javascript_dialog_title = [this] {
        auto origin = view().url().origin();
        if (!origin.is_opaque())
            return qstring_from_ak_string(origin.serialize());
        return qformatted("{}://", qstring_from_ak_string(view().url().scheme()));
    };

    view().on_request_alert = [this, javascript_dialog_title](auto const& message) {
        if (m_suppress_javascript_dialogs_until_navigation) {
            view().alert_closed();
            return;
        }
        hide_hover_label();
        m_javascript_dialog->show_alert(javascript_dialog_title(), qstring_from_utf16_string(message));
    };

    view().on_request_before_unload = [this](auto const& source, auto on_complete) {
        hide_hover_label();
        m_before_unload_completion = AK::move(on_complete);
        m_window->raise_for_close_prompt();
        m_javascript_dialog->show_before_unload(qstring_from_ak_string(source));
    };

    view().on_request_confirm = [this, javascript_dialog_title](auto const& message) {
        if (m_suppress_javascript_dialogs_until_navigation) {
            view().confirm_closed(false);
            return;
        }
        hide_hover_label();
        m_javascript_dialog->show_confirm(javascript_dialog_title(), qstring_from_utf16_string(message));
    };

    view().on_request_prompt = [this, javascript_dialog_title](auto const& message, auto const& default_) {
        if (m_suppress_javascript_dialogs_until_navigation) {
            view().prompt_closed({});
            return;
        }
        hide_hover_label();
        m_javascript_dialog->show_prompt(javascript_dialog_title(), qstring_from_utf16_string(message), qstring_from_utf16_string(default_));
    };

    view().on_request_set_prompt_text = [this](auto const& message) {
        m_javascript_dialog->set_prompt_text(qstring_from_utf16_string(message));
    };

    m_javascript_dialog->on_complete = [this](auto type, bool accepted, auto const& prompt_text) {
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
        if (m_file_picker_dialog) {
            auto* dialog = m_file_picker_dialog.data();
            m_file_picker_dialog = nullptr;
            QObject::disconnect(dialog, nullptr, this, nullptr);
            dialog->close();
        }
        if (m_external_url_confirmation_dialog)
            m_external_url_confirmation_dialog->close();
        auto pending_permission_requests = AK::move(m_pending_permission_requests);
        for (auto& it : pending_permission_requests) {
            if (it.value.dialog)
                it.value.dialog->close();
        }
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

    view().on_request_permission = [this](auto const& name, auto const& origin, auto on_decided) {
        auto requester = origin.is_opaque() ? "this page"_string : origin.serialize();
        auto question = MUST(String::formatted("Allow {} to use your {}?", requester, name));
        if (auto pending = m_pending_permission_requests.get(question); pending.has_value()) {
            pending.value().on_decided.append(AK::move(on_decided));
            return;
        }

        auto* dialog = new QMessageBox(&view());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setIcon(QMessageBox::Icon::Question);
        dialog->setWindowTitle("Ladybird");
        dialog->setTextFormat(Qt::PlainText);
        dialog->setText(qstring_from_ak_string(question));
        auto* allow_button = dialog->addButton("Allow", QMessageBox::ButtonRole::AcceptRole);
        auto* block_button = dialog->addButton("Block", QMessageBox::ButtonRole::RejectRole);
        dialog->setDefaultButton(block_button);

        PendingPermissionRequests pending { .dialog = dialog, .on_decided = {} };
        pending.on_decided.append(AK::move(on_decided));
        m_pending_permission_requests.set(question, AK::move(pending));

        QObject::connect(dialog, &QDialog::finished, this, [this, dialog = QPointer<QMessageBox> { dialog }, allow_button, question](auto) {
            auto pending = m_pending_permission_requests.take(question);
            if (!pending.has_value())
                return;
            auto granted = dialog && dialog->clickedButton() == allow_button;
            for (auto& on_decided : pending.value().on_decided)
                on_decided(granted);
        });

        dialog->open();
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

    QObject::connect(focus_location_editor_action, &QAction::triggered, this, &Tab::focus_location_editor);

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

    view().on_audio_play_state_changed = [this](auto play_state) {
        emit audio_play_state_changed(tab_index(), play_state);
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
            if (!m_window->tab_at(i)->is_pinned())
                m_window->request_to_close_tab(i);
        }
    });

    auto* close_tabs_to_right_action = new QAction("Close Tabs to R&ight", this);
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
    m_context_menu->addAction(create_application_action(*this, application.reload_action(), IncludeActionIcon::No));
    m_context_menu->addAction(duplicate_tab_action);
    m_context_menu->addAction(m_pin_tab_action);
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
    // initialize() hides the toolbar's window controls, which overwrites the visibility TabWidget last asked for.
    auto toolbar_window_controls_visible = m_toolbar_window_controls_visible;
    initialize({});
    m_deferred_tab.clear();

    // TabWidget pushed these while the tab had no toolbar to apply them to.
    set_vertical_tabs_enabled(m_vertical_tabs_enabled);
    recreate_toolbar_icons();
    set_toolbar_window_controls_visible(toolbar_window_controls_visible);

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

void Tab::focus_location_editor()
{
    if (!is_loaded())
        return;

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

    if (!is_loaded()) {
        m_window = &window;
        return;
    }

    QObject::disconnect(&m_window->hamburger_menu(), nullptr, m_hamburger_button, nullptr);

    m_window = &window;
    m_hamburger_button->setMenu(&m_window->hamburger_menu());
    connect_hamburger_menu();
    recreate_toolbar_icons();

    // The tab's pages hold the geometry of the window that hosted them until now.
    m_view->set_window_position({ window.x(), window.y() });
    m_view->set_window_size({ window.width(), window.height() });
}

void Tab::set_pinned(bool pinned)
{
    m_is_pinned = pinned;
    m_pin_tab_action->setText(pinned ? "Un&pin Tab" : "&Pin Tab");
}

void Tab::set_vertical_tabs_enabled(bool enabled)
{
    m_vertical_tabs_enabled = enabled;
    if (!is_loaded())
        return;

    m_toolbar->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    update_bookmarks_bar_visibility();
    m_toolbar->layout()->invalidate();
}

#ifdef AK_OS_MACOS
void Tab::update_web_view_frame(bool chrome_visible, Optional<WebView::VerticalTabsPosition> sidebar_position)
{
    m_web_view_chrome_visible = chrome_visible;
    m_web_view_sidebar_position = sidebar_position;
    update();
    if (!is_loaded())
        return;

    // With the browser chrome hidden, the page fills the window edge to edge.
    if (!chrome_visible) {
        m_view_layout->setContentsMargins(0, 0, 0, 0);
        m_view->set_has_rounded_corners(false);
        return;
    }

    // Keep the web view's rounded corners clear of the window edges.
    auto left = sidebar_position == WebView::VerticalTabsPosition::Left ? WEB_VIEW_SIDEBAR_INSET : WEB_VIEW_INSET;
    auto right = sidebar_position == WebView::VerticalTabsPosition::Right ? WEB_VIEW_SIDEBAR_INSET : WEB_VIEW_INSET;
    m_view_layout->setContentsMargins(left, 0, right, WEB_VIEW_INSET);
    m_view->set_has_rounded_corners(true);
}
#endif

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

// NB: The location field gets a point more room above it below the horizontal tab strip, where the selected tab flows
//     into the toolbar, and at the top of a macOS window, whose outline covers the toolbar's first point.
static int toolbar_extra_top_margin(bool window_controls_visible)
{
    return use_native_macos_window_controls() || !window_controls_visible ? 1 : 0;
}

int Tab::toolbar_height(bool window_controls_visible)
{
    return browser_chrome_layout_policy().toolbar_height + toolbar_extra_top_margin(window_controls_visible) + (TOOLBAR_BOTTOM_MARGIN - TOOLBAR_VERTICAL_MARGIN);
}

void Tab::set_toolbar_window_controls_visible(bool visible)
{
    m_toolbar_window_controls_visible = visible;
    if (!is_loaded())
        return;

    auto const has_native_window_controls = use_native_macos_window_controls() && visible;
    auto const has_custom_window_controls = !use_native_macos_window_controls() && visible;

    m_toolbar_window_controls_separator->setVisible(has_custom_window_controls);
    m_toolbar_window_controls->setVisible(has_native_window_controls || has_custom_window_controls);
    m_toolbar->setFixedHeight(toolbar_height(visible));
    m_toolbar->layout()->setContentsMargins(TOOLBAR_HORIZONTAL_MARGIN, TOOLBAR_VERTICAL_MARGIN + toolbar_extra_top_margin(visible), has_custom_window_controls ? TOOLBAR_WINDOW_CONTROLS_RIGHT_MARGIN : TOOLBAR_HORIZONTAL_MARGIN, TOOLBAR_BOTTOM_MARGIN);
    m_toolbar->layout()->invalidate();
}

void Tab::update_window_control_icons()
{
    if (!m_minimize_window_button || !m_maximize_window_button || !m_close_window_button)
        return;

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

QIcon Tab::tab_icon() const
{
    if (m_is_loading)
        return loading_spinner_icon(palette(), m_loading_animation_frame);
    return m_favicon;
}

QString Tab::title() const
{
    auto title = m_fixed_title.value_or(m_title);
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

    auto const& tab_settings = WebView::Application::settings().tab_settings();
    m_vertical_tabs_enabled = tab_settings.vertical_tabs_enabled;
    recreate_toolbar_icons();
    update_bookmarks_bar_visibility();
    m_toolbar->layout()->invalidate();
}

void Tab::config_variable_changed(WebView::ConfigVariableID variable)
{
    if (!is_loaded())
        return;

    if (variable == WebView::ConfigVariableID::ShowTabPerformanceMonitor)
        m_performance_monitor->setVisible(WebView::Application::settings().config_variable_as_bool(variable));
    if (variable == WebView::ConfigVariableID::ShowWebContentProcessIDInTabTitle)
        update_tab_title();
}

void Tab::content_blocker_count_visibility_changed()
{
    if (!is_loaded())
        return;

    m_content_blocking_button->set_show_count_badge(WebView::Application::settings().show_content_blocker_count());
}

void Tab::set_loading(bool is_loading)
{
    if (m_is_loading == is_loading)
        return;

    m_is_loading = is_loading;
    m_loading_animation_frame = 0;
    emit loading_changed();

    if (m_is_loading)
        m_loading_animation_timer->start();
    else
        m_loading_animation_timer->stop();

    update_tab_icon();

    m_reload_button->set_loading(m_is_loading);

    // Only mark the end of loads that finished, rather than being stopped. The browser's own about: pages are not
    // marked.
    if (!m_is_loading && view().last_load_finished() && view().url().scheme() != "about"sv)
        emit loading_finished();
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

#ifdef AK_OS_MACOS
void Tab::paintEvent(QPaintEvent*)
{
    // The inset around the web view should match the chrome surrounding it.
    QPainter painter(this);
    auto const& palette = window()->palette();
    painter.fillRect(rect(), ChromeStyle::chrome_background(palette));

    if (!m_web_view_chrome_visible || !m_web_view_sidebar_position.has_value() || !is_loaded())
        return;

    // At the bottom of the window, the sidebar's color reaches around the web view's rounded corner beside it, so the
    // inset bends around that corner rather than meeting the sidebar in a square corner. At the top, the inset joins
    // the toolbar instead.
    // NB: The curve meets the sidebar's edge and the window's bottom edge in a tangent.
    auto radius = m_view->viewport_corner_radius() + WEB_VIEW_SIDEBAR_INSET;
    auto left = *m_web_view_sidebar_position == WebView::VerticalTabsPosition::Right ? width() - radius : 0.0;
    auto curve_left = *m_web_view_sidebar_position == WebView::VerticalTabsPosition::Right ? width() - (2 * radius) : 0.0;

    QPainterPath corner;
    corner.addRect(left, height() - radius, radius, radius);
    QPainterPath inside;
    inside.addEllipse(curve_left, height() - (2 * radius), 2 * radius, 2 * radius);

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillPath(corner.subtracted(inside), ChromeStyle::chrome_tab_strip_background(palette));
}
#endif

void Tab::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (isVisible())
        did_get_real_size();
    if (!is_loaded())
        return;

    if (m_hover_label->isVisible())
        update_hover_label();
}

void Tab::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (!is_loaded())
        return;

    set_bookmarks_bar_hover_expanded(false);
    m_hover_label->hide();
    if (m_content_blocking_popover)
        m_content_blocking_popover->close();
}

void Tab::update_hover_label()
{
    m_hover_label->setText(QFontMetrics(m_hover_label->font()).elidedText(m_hover_label->text(), Qt::ElideRight, width() / 2 - 10));
    m_hover_label->resize(m_hover_label->sizeHint());

    auto hover_label_height = height() - m_hover_label->height();
    if (m_find_in_page->isVisible())
        hover_label_height -= m_find_in_page->height();

    auto left_position = mapToGlobal(QPoint { 0, hover_label_height });

    if (m_hover_label->underMouse() && m_hover_label->pos() == left_position)
        m_hover_label->move(mapToGlobal(QPoint { width() - m_hover_label->width(), hover_label_height }));
    else
        m_hover_label->move(left_position);

    m_hover_label->raise();
}

void Tab::hide_hover_label()
{
    m_hover_label->hide();
}

bool Tab::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange && is_loaded()) {
        update_chrome_style();
        recreate_toolbar_icons();
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
    if (watched == m_view && (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        update_bookmarks_bar_geometry();
    if (watched == m_bookmarks_bar_container && event->type() == QEvent::Resize)
        update_bookmarks_bar_geometry();
    if (watched == m_bookmarks_bar && event->type() == QEvent::LayoutRequest)
        update_bookmarks_bar_geometry();

    if (watched == m_toolbar || watched == m_bookmarks_bar_container) {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Show) {
            update_bookmarks_bar_hover();
        } else if (event->type() == QEvent::Leave) {
            QTimer::singleShot(0, this, &Tab::update_bookmarks_bar_hover);
        } else if (event->type() == QEvent::Hide && watched == m_toolbar) {
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
    m_toolbar_container->setStyleSheet(ChromeStyle::toolbar_container_style_sheet(palette()));
    m_bookmarks_bar->update_chrome_style();
    auto hover_surface = ChromeStyle::style_sheet_color(ChromeStyle::chrome_surface(palette()));
    auto hover_border = ChromeStyle::style_sheet_color(ChromeStyle::chrome_border(palette()));
    auto hover_text = ChromeStyle::style_sheet_color(ChromeStyle::chrome_text(palette()));
    m_hover_label->setStyleSheet(qformatted("background: {}; color: {}; border: 1px solid {}; border-radius: 6px;",
        hover_surface, hover_text, hover_border));
    if (m_downloads_popover)
        m_downloads_popover->update_chrome_style(palette());
    if (m_content_blocking_popover)
        m_content_blocking_popover->update_chrome_style(palette());
    m_is_updating_chrome_style = false;
}

void Tab::recreate_toolbar_icons()
{
    m_navigate_back_action->setIcon(create_chrome_icon(ChromeIcon::Back, palette()));
    m_navigate_forward_action->setIcon(create_chrome_icon(ChromeIcon::Forward, palette()));
    m_reload_action->setIcon(create_chrome_icon(ChromeIcon::Reload, palette()));
    m_stop_action->setIcon(create_chrome_icon(ChromeIcon::Stop, palette()));
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

    auto downloads = WebView::Application::the().file_downloader().downloads_of_session(m_window->is_private());
    auto button_state = WebView::downloads_button_state(downloads);

    m_downloads_button->setVisible(button_state.has_downloads);
    if (!button_state.has_downloads) {
        static_cast<DownloadsButton*>(m_downloads_button)->set_progress({});
        if (m_downloads_popover)
            m_downloads_popover->close();
    }

    auto downloads_icon = button_state.active_download_count > 0 && !button_state.active_download_progress.has_value() ? ChromeIcon::DownloadActive : ChromeIcon::Download;
    if (!m_downloads_button_icon.has_value() || *m_downloads_button_icon != downloads_icon) {
        auto icon = create_chrome_icon(downloads_icon, palette());
        m_open_downloads_page_action->setIcon(icon);
        m_downloads_button->setIcon(icon);
        static_cast<DownloadsButton*>(m_downloads_button)->set_progress_icon(create_chrome_icon(ChromeIcon::DownloadActive, palette()));
        m_downloads_button_icon = downloads_icon;
    }

    static_cast<DownloadsButton*>(m_downloads_button)->set_progress(button_state.active_download_count > 0 ? button_state.active_download_progress : Optional<double> {});

    auto tooltip = qstring_from_ak_string(button_state.tooltip);
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

    if (m_downloads_popover->set_downloads(WebView::Application::the().file_downloader().downloads_of_session(m_window->is_private())))
        position_downloads_popover();
}

void Tab::show_downloads_popover()
{
    if (!m_downloads_button || !m_downloads_button->isVisible())
        return;

    if (!m_downloads_popover) {
        m_downloads_popover = new DownloadsPopover(this);
        m_downloads_popover->on_visibility_change = [button = QPointer(m_downloads_button)](bool visible) {
            if (button)
                button->setDown(visible);
        };
        m_downloads_popover->on_cancel_download = [this](u64 id) {
            auto& file_downloader = WebView::Application::the().file_downloader();
            if (auto download = file_downloader.download(id); download.has_value() && WebView::FileDownloader::status_is_active(download->status))
                file_downloader.cancel_download(id);
            update_downloads_popover();
        };
        m_downloads_popover->on_pause_download = [this](u64 id) {
            WebView::Application::the().file_downloader().pause_download(id);
            update_downloads_popover();
        };
        m_downloads_popover->on_resume_download = [this](u64 id) {
            WebView::Application::the().file_downloader().resume_download(id);
            update_downloads_popover();
        };
        m_downloads_popover->on_open_all_downloads = [this] {
            if (m_downloads_popover)
                m_downloads_popover->close();
            m_open_downloads_page_action->trigger();
        };
    }

    m_downloads_popover->update_chrome_style(palette());
    (void)m_downloads_popover->set_downloads(WebView::Application::the().file_downloader().downloads_of_session(m_window->is_private()));
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
    auto& card = m_downloads_popover->card();
    auto size = card.sizeHint();
    size.setWidth(DOWNLOADS_POPOVER_WIDTH);
    if (size.height() > DOWNLOADS_POPOVER_MAX_HEIGHT)
        size.setHeight(DOWNLOADS_POPOVER_MAX_HEIGHT);
    card.setFixedSize(size);
    m_downloads_popover->adjustSize();

    move_popover_below(*m_downloads_popover, *m_downloads_button);
}

void Tab::download_added(WebView::FileDownloader::Download const& download)
{
    if (!is_loaded())
        return;

    // NB: A window lists the downloads of its own browsing session only.
    if (download.is_private != m_window->is_private())
        return;
    update_downloads_button();
    if (Application::the().active_tab() == this && m_window->isActiveWindow()) {
        show_downloads_popover();
        return;
    }
    update_downloads_popover();
}

void Tab::download_updated(WebView::FileDownloader::Download const& download)
{
    if (!is_loaded())
        return;

    if (download.is_private != m_window->is_private())
        return;
    update_downloads_button();
    update_downloads_popover();
}

void Tab::download_removed(u64, WebView::IsPrivate is_private)
{
    if (!is_loaded())
        return;

    if (is_private != m_window->is_private())
        return;
    update_downloads_button();
    update_downloads_popover();
}

void Tab::update_content_blocking_controls(WebView::ContentBlockingSnapshot const& snapshot)
{
    m_content_blocking_button->set_snapshot(snapshot);
    if (!m_content_blocking_popover || !m_content_blocking_popover->isVisible())
        return;

    if (snapshot.state == WebView::ContentBlockingState::Unavailable
        || (m_content_blocking_popover_reload_site.has_value() && snapshot.site != m_content_blocking_popover_reload_site)) {
        m_content_blocking_popover->close();
        return;
    }

    m_content_blocking_popover->set_snapshot(snapshot);
    move_popover_below(*m_content_blocking_popover, *m_content_blocking_button);
}

void Tab::show_content_blocking_popover()
{
    if (m_content_blocking_popover && m_content_blocking_popover->isVisible()) {
        m_content_blocking_popover->close();
        m_content_blocking_button->setFocus(Qt::PopupFocusReason);
        return;
    }

    auto snapshot = view().content_blocking_snapshot();
    if (snapshot.state == WebView::ContentBlockingState::Unavailable)
        return;

    if (!m_content_blocking_popover) {
        m_content_blocking_popover = new ContentBlockingPopover(this);
        m_content_blocking_popover->on_visibility_change = [this, button = QPointer(m_content_blocking_button)](bool visible) {
            if (button)
                button->setDown(visible);
            if (!visible)
                m_content_blocking_popover_reload_site.clear();
        };
        m_content_blocking_popover->on_keyboard_dismiss = [button = QPointer(m_content_blocking_button)] {
            if (button && button->isVisible())
                button->setFocus(Qt::PopupFocusReason);
        };
    }

    m_content_blocking_popover->on_toggle = [this, site = snapshot.site](bool enabled) {
        auto current = view().content_blocking_snapshot();
        if (!site.has_value() || current.site != site || !current.can_toggle()) {
            m_content_blocking_popover->close();
            return;
        }

        if (auto result = WebView::Application::settings().set_content_blocker_enabled_for_site(*site, enabled); result.is_error()) {
            m_content_blocking_popover->set_snapshot(view().content_blocking_snapshot());
            m_content_blocking_popover->show_error("Unable to save the site preference.");
            move_popover_below(*m_content_blocking_popover, *m_content_blocking_button);
            return;
        }

        m_content_blocking_popover_reload_site = site;
        view().reload();
    };

    m_content_blocking_popover->update_chrome_style(palette());
    m_content_blocking_popover->set_favicon(m_favicon);
    m_content_blocking_popover->set_snapshot(snapshot);
    move_popover_below(*m_content_blocking_popover, *m_content_blocking_button);
    m_content_blocking_popover->show();
    move_popover_below(*m_content_blocking_popover, *m_content_blocking_button);
    m_content_blocking_popover->raise();
    m_content_blocking_popover->focus_switch();
}

void Tab::show_private_session_popover()
{
    if (!m_private_badge || !m_private_badge->isVisible())
        return;

    if (!m_private_session_popover) {
        m_private_session_popover = new MessagePopover(this, PRIVATE_SESSION_POPOVER_WIDTH, "Start a fresh private session?",
            "This closes all private windows and deletes history and other site data from the current private browsing session.",
            "Cancel", "Restart Private Session");
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
    if (!m_private_session_popover || !m_private_badge)
        return;

    m_private_session_popover->adjustSize();

    move_popover_below(*m_private_session_popover, *m_private_badge);
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

void Tab::update_bookmarks_bar_visibility()
{
    auto show_bookmarks_bar = Application::settings().appearance().show_bookmarks_bar;
    auto* toolbar_layout = static_cast<QBoxLayout*>(m_toolbar_container->layout());

    if (show_bookmarks_bar == WebView::ShowBookmarksBar::OnHover) {
        // Hovering must not resize the page or move the sidebar. Keep the bar out of both layouts and overlay the view.
        toolbar_layout->removeWidget(m_bookmarks_bar_container);
        static_cast<QBoxLayout*>(layout())->removeWidget(m_bookmarks_bar_container);
        if (m_bookmarks_bar_container->parentWidget() != this)
            m_bookmarks_bar_container->setParent(this);
    } else if (toolbar_layout->indexOf(m_bookmarks_bar_container) == -1) {
        // An always-visible bar remains in the full-width toolbar above the vertical tabs.
        toolbar_layout->addWidget(m_bookmarks_bar_container);
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
        && isVisible() && m_toolbar->isVisible() && !m_window->isFullScreen()
        && !m_location_edit->property("autocompleteOpen").toBool();
}

bool Tab::cursor_is_over_bookmarks_bar_hover_region() const
{
    // Menus are separate windows, so QWidget::isAncestorOf() does not follow them back to the toolbar.
    for (auto* popup = QApplication::activePopupWidget(); popup; popup = popup->parentWidget()) {
        if (popup == m_bookmarks_bar)
            return true;
    }

    if (!m_window->underMouse())
        return false;

    auto cursor_position = QCursor::pos();
    auto contains_cursor = [&](QWidget const& widget) {
        return widget.isVisible() && QRect { widget.mapToGlobal(QPoint {}), widget.size() }.contains(cursor_position);
    };
    return contains_cursor(*m_toolbar) || contains_cursor(*m_bookmarks_bar_container);
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

    auto duration = expanded
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

    auto hover_bookmarks_visible = on_hover && visible;
    if (m_toolbar_container->property("hoverBookmarksVisible").toBool() != hover_bookmarks_visible) {
        m_toolbar_container->setProperty("hoverBookmarksVisible", hover_bookmarks_visible);
        m_toolbar_container->style()->unpolish(m_toolbar_container);
        m_toolbar_container->style()->polish(m_toolbar_container);
        m_toolbar_container->update();
    }
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
