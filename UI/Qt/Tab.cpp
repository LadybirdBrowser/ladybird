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
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/ChromeLayout.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/WindowControlButton.h>

#include <QColorDialog>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QResizeEvent>
#include <QTimer>

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
    navigation_button_layout->addWidget(create_toolbar_button(*navigation_button_cluster, *m_toggle_vertical_tabs_expanded_action));
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
    toolbar_layout->addWidget(m_hamburger_button, 0, Qt::AlignTop);
    if (use_right_custom_window_controls()) {
        toolbar_layout->addWidget(m_toolbar_window_controls_separator, 0, Qt::AlignVCenter);
        toolbar_layout->addWidget(m_toolbar_window_controls, 0, Qt::AlignVCenter);
    }

    update_chrome_style();
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
    m_location_edit->setFocus();
    m_location_edit->selectAll();
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
    m_toolbar->setProperty(WINDOW_DRAG_REGION_PROPERTY, true);
    if (m_sidebar_toggle_navigation_spacer)
        m_sidebar_toggle_navigation_spacer->changeSize(enabled ? TOOLBAR_SIDEBAR_TOGGLE_NAVIGATION_GAP : 0, 0, QSizePolicy::Fixed, QSizePolicy::Minimum);
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
    m_is_updating_chrome_style = false;
}

void Tab::recreate_toolbar_icons()
{
    m_toggle_vertical_tabs_expanded_action->setIcon(create_chrome_icon(
        WebView::Application::settings().tab_settings().vertical_tabs_expanded
            ? ChromeIcon::VerticalTabBarCollapse
            : ChromeIcon::VerticalTabBarExpand,
        palette()));
    m_navigate_back_action->setIcon(create_chrome_icon(ChromeIcon::Back, palette()));
    m_navigate_forward_action->setIcon(create_chrome_icon(ChromeIcon::Forward, palette()));
    m_reload_action->setIcon(create_chrome_icon(ChromeIcon::Reload, palette()));
    m_hamburger_button->setIcon(create_chrome_icon(ChromeIcon::Menu, palette()));
    update_window_control_icons();

    if (auto* action = m_location_edit->trailing_action()) {
        auto icon = view().toggle_bookmark_action().engaged() ? ChromeIcon::StarFilled : ChromeIcon::Star;
        action->setIcon(create_chrome_icon(icon, palette()));
    }
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
        m_window->definitely_close_tab(tab_index());
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
