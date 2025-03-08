/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibGfx/ImageFormats/BMPWriter.h>
#include <LibURL/Parser.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/SourceHighlighter.h>
#include <LibWebView/URL.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/InspectorWidget.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/StringUtils.h>

#include <QClipboard>
#include <QColorDialog>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMimeType>
#include <QPainter>
#include <QPoint>
#include <QPushButton>
#include <QResizeEvent>

namespace Ladybird {

static QIcon default_favicon()
{
    static QIcon icon = load_icon_from_uri("resource://icons/48x48/app-browser.png"sv);
    return icon;
}

Tab::Tab(BrowserWindow* window, RefPtr<WebView::WebContentClient> parent_client, size_t page_index)
    : QWidget(window)
    , m_window(window)
{
    m_layout = new QBoxLayout(QBoxLayout::Direction::TopToBottom, this);
    m_layout->setSpacing(0);
    m_layout->setContentsMargins(0, 0, 0, 0);

    m_view = new WebContentView(this, parent_client, page_index);
    m_find_in_page = new FindInPageWidget(this, m_view);
    m_find_in_page->setVisible(false);
    m_toolbar = new QToolBar(this);
    m_location_edit = new LocationEdit(this);

    m_hover_label = new HyperlinkLabel(this);
    m_hover_label->hide();
    m_hover_label->setFrameShape(QFrame::Shape::Box);
    m_hover_label->setAutoFillBackground(true);

    QObject::connect(m_hover_label, &HyperlinkLabel::mouse_entered, [this] {
        update_hover_label();
    });

    auto* focus_location_editor_action = new QAction("Edit Location", this);
    focus_location_editor_action->setShortcut(QKeySequence("Ctrl+L"));
    addAction(focus_location_editor_action);

    m_layout->addWidget(m_toolbar);
    m_layout->addWidget(m_view);
    m_layout->addWidget(m_find_in_page);

    m_hamburger_button = new QToolButton(m_toolbar);
    m_hamburger_button->setText("Show &Menu");
    m_hamburger_button->setToolTip("Show Menu");
    m_hamburger_button->setIcon(create_tvg_icon_with_theme_colors("hamburger", palette()));
    m_hamburger_button->setPopupMode(QToolButton::InstantPopup);
    m_hamburger_button->setMenu(&m_window->hamburger_menu());
    m_hamburger_button->setStyleSheet(":menu-indicator {image: none}");

    recreate_toolbar_icons();

    m_favicon = default_favicon();

    m_toolbar->addAction(&m_window->go_back_action());
    m_toolbar->addAction(&m_window->go_forward_action());
    m_toolbar->addAction(&m_window->reload_action());
    m_toolbar->addWidget(m_location_edit);
    m_toolbar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    m_hamburger_button_action = m_toolbar->addWidget(m_hamburger_button);
    m_toolbar->setIconSize({ 16, 16 });
    // This is a little awkward, but without this Qt shrinks the button to the size of the icon.
    // Note: toolButtonStyle="0" -> ToolButtonIconOnly.
    m_toolbar->setStyleSheet("QToolButton[toolButtonStyle=\"0\"]{width:24px;height:24px}");

    m_hamburger_button_action->setVisible(!Settings::the()->show_menubar());

    QObject::connect(Settings::the(), &Settings::show_menubar_changed, this, [this](bool show_menubar) {
        m_hamburger_button_action->setVisible(!show_menubar);
    });

    m_reset_zoom_button = new QToolButton(m_toolbar);
    m_reset_zoom_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_reset_zoom_button->setToolTip("Reset zoom level");
    m_reset_zoom_button_action = m_toolbar->addWidget(m_reset_zoom_button);
    m_reset_zoom_button_action->setVisible(false);

    QObject::connect(m_reset_zoom_button, &QAbstractButton::clicked, [this] {
        view().reset_zoom();
        update_reset_zoom_button();
        m_window->update_zoom_menu();
    });

    view().on_activate_tab = [this] {
        m_window->activate_tab(tab_index());
    };

    view().on_close = [this] {
        m_window->close_tab(tab_index());
    };

    view().on_link_hover = [this](auto const& url) {
        m_hover_label->setText(qstring_from_ak_string(url.to_byte_string()));
        update_hover_label();
        m_hover_label->show();
    };

    view().on_link_unhover = [this]() {
        m_hover_label->hide();
    };

    view().on_load_start = [this](const URL::URL& url, bool) {
        if (m_inspector_widget)
            m_inspector_widget->reset();

        auto url_serialized = qstring_from_ak_string(url.serialize());

        m_title = url_serialized;
        emit title_changed(tab_index(), url_serialized);

        m_favicon = default_favicon();
        emit favicon_changed(tab_index(), m_favicon);

        m_location_edit->set_url(url);
        m_location_edit->setCursorPosition(0);
    };

    view().on_load_finish = [this](auto&) {
        if (m_inspector_widget != nullptr && m_inspector_widget->isVisible())
            m_inspector_widget->inspect();
    };

    view().on_url_change = [this](auto const& url) {
        m_location_edit->set_url(url);
    };

    QObject::connect(m_location_edit, &QLineEdit::returnPressed, this, &Tab::location_edit_return_pressed);

    view().on_title_change = [this](auto const& title) {
        m_title = qstring_from_ak_string(title);

        emit title_changed(tab_index(), m_title);
    };

    view().on_favicon_change = [this](auto const& bitmap) {
        auto qimage = QImage(bitmap.scanline_u8(0), bitmap.width(), bitmap.height(), QImage::Format_ARGB32);
        if (qimage.isNull())
            return;
        auto qpixmap = QPixmap::fromImage(qimage);
        if (qpixmap.isNull())
            return;

        m_favicon = qpixmap;
        emit favicon_changed(tab_index(), m_favicon);
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

            if (auto file = Web::HTML::SelectedFile::from_file_path(file_path); file.is_error())
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

                    accepted_file_filters.append(QString("%1 (%2)").arg(title, extensions.join(" ")));
                },
                [&](Web::HTML::FileFilter::MimeType const& filter) {
                    if (auto mime_type = mime_database.mimeTypeForName(qstring_from_ak_string(filter.value)); mime_type.isValid())
                        accepted_file_filters.append(mime_type.filterString());
                },
                [&](Web::HTML::FileFilter::Extension const& filter) {
                    auto extension = MUST(String::formatted("*.{}", filter.value));
                    accepted_file_filters.append(qstring_from_ak_string(extension));
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

    view().on_received_source = [this](auto const& url, auto const& base_url, auto const& source) {
        auto html = WebView::highlight_source(url, base_url, source, Syntax::Language::HTML, WebView::HighlightOutputMode::FullDocument);
        m_window->new_tab_from_content(html, Web::HTML::ActivateTab::Yes);
    };

    view().on_inspector_requested_style_sheet_source = [this](auto const& identifier) {
        view().request_style_sheet_source(identifier);
    };

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
        m_window->showFullScreen();
        view().did_update_window_rect();
    };

    view().on_insert_clipboard_entry = [](auto const& data, auto const&, auto const& mime_type) {
        QByteArray qdata { data.bytes_as_string_view().characters_without_null_termination(), static_cast<qsizetype>(data.bytes_as_string_view().length()) };

        auto* mime_data = new QMimeData();
        mime_data->setData(qstring_from_ak_string(mime_type), qdata);

        auto* clipboard = QGuiApplication::clipboard();
        clipboard->setMimeData(mime_data);
    };

    view().on_audio_play_state_changed = [this](auto play_state) {
        emit audio_play_state_changed(tab_index(), play_state);
    };

    view().on_navigation_buttons_state_changed = [this](auto back_enabled, auto forward_enabled) {
        m_can_navigate_back = back_enabled;
        m_can_navigate_forward = forward_enabled;
        emit navigation_buttons_state_changed(tab_index());
    };

    auto* reload_tab_action = new QAction("&Reload Tab", this);
    QObject::connect(reload_tab_action, &QAction::triggered, this, [this]() {
        reload();
    });

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
        view().on_close();
    });

    auto* close_tabs_to_left_action = new QAction("C&lose Tabs to Left", this);
    QObject::connect(close_tabs_to_left_action, &QAction::triggered, this, [this]() {
        for (auto i = tab_index() - 1; i >= 0; i--) {
            m_window->close_tab(i);
        }
    });

    auto* close_tabs_to_right_action = new QAction("Close Tabs to R&ight", this);
    QObject::connect(close_tabs_to_right_action, &QAction::triggered, this, [this]() {
        for (auto i = m_window->tab_count() - 1; i > tab_index(); i--) {
            m_window->close_tab(i);
        }
    });

    auto* close_other_tabs_action = new QAction("Cl&ose Other Tabs", this);
    QObject::connect(close_other_tabs_action, &QAction::triggered, this, [this]() {
        for (auto i = m_window->tab_count() - 1; i >= 0; i--) {
            if (i == tab_index())
                continue;

            m_window->close_tab(i);
        }
    });

    m_context_menu = new QMenu("Context menu", this);
    m_context_menu->addAction(reload_tab_action);
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

    auto* search_selected_text_action = new QAction("&Search for <query>", this);
    search_selected_text_action->setIcon(load_icon_from_uri("resource://icons/16x16/find.png"sv));
    QObject::connect(search_selected_text_action, &QAction::triggered, this, [this]() {
        auto url_string = MUST(String::formatted(Settings::the()->search_engine().query_url, URL::percent_encode(*m_page_context_menu_search_text)));
        auto url = URL::Parser::basic_parse(url_string);
        VERIFY(url.has_value());
        m_window->new_tab_from_url(url.release_value(), Web::HTML::ActivateTab::Yes);
    });

    auto take_screenshot = [this](auto type) {
        auto& view = this->view();

        view.take_screenshot(type)
            ->when_resolved([this](auto const& path) {
                auto message = MUST(String::formatted("Screenshot saved to: {}", path));

                QMessageBox dialog(this);
                dialog.setWindowTitle("Ladybird");
                dialog.setIcon(QMessageBox::Information);
                dialog.setText(qstring_from_ak_string(message));
                dialog.addButton(QMessageBox::Ok);
                dialog.addButton(QMessageBox::Open)->setText("Open folder");

                if (dialog.exec() == QMessageBox::Open) {
                    auto path_url = QUrl::fromLocalFile(qstring_from_ak_string(path.dirname()));
                    QDesktopServices::openUrl(path_url);
                }
            })
            .when_rejected([this](auto const& error) {
                if (error.is_errno() && error.code() == ECANCELED)
                    return;

                auto error_message = MUST(String::formatted("{}", error));
                QMessageBox::warning(this, "Ladybird", qstring_from_ak_string(error_message));
            });
    };

    auto* take_visible_screenshot_action = new QAction("Take &Visible Screenshot", this);
    take_visible_screenshot_action->setIcon(load_icon_from_uri("resource://icons/16x16/filetype-image.png"sv));
    QObject::connect(take_visible_screenshot_action, &QAction::triggered, this, [take_screenshot]() {
        take_screenshot(WebView::ViewImplementation::ScreenshotType::Visible);
    });

    auto* take_full_screenshot_action = new QAction("Take &Full Screenshot", this);
    take_full_screenshot_action->setIcon(load_icon_from_uri("resource://icons/16x16/filetype-image.png"sv));
    QObject::connect(take_full_screenshot_action, &QAction::triggered, this, [take_screenshot]() {
        take_screenshot(WebView::ViewImplementation::ScreenshotType::Full);
    });

    m_page_context_menu = new QMenu("Context menu", this);
    m_page_context_menu->addAction(&m_window->go_back_action());
    m_page_context_menu->addAction(&m_window->go_forward_action());
    m_page_context_menu->addAction(&m_window->reload_action());
    m_page_context_menu->addSeparator();
    m_page_context_menu->addAction(&m_window->copy_selection_action());
    m_page_context_menu->addAction(&m_window->paste_action());
    m_page_context_menu->addAction(&m_window->select_all_action());
    m_page_context_menu->addSeparator();
    m_page_context_menu->addAction(search_selected_text_action);
    m_page_context_menu->addSeparator();
    m_page_context_menu->addAction(take_visible_screenshot_action);
    m_page_context_menu->addAction(take_full_screenshot_action);
    m_page_context_menu->addSeparator();
    m_page_context_menu->addAction(&m_window->view_source_action());
    m_page_context_menu->addAction(&m_window->inspect_dom_node_action());

    view().on_context_menu_request = [this, search_selected_text_action](Gfx::IntPoint content_position) {
        auto selected_text = Settings::the()->enable_search()
            ? view().selected_text_with_whitespace_collapsed()
            : OptionalNone {};
        TemporaryChange change_url { m_page_context_menu_search_text, std::move(selected_text) };

        if (m_page_context_menu_search_text.has_value()) {
            auto action_text = WebView::format_search_query_for_display(Settings::the()->search_engine().query_url, *m_page_context_menu_search_text);
            search_selected_text_action->setText(qstring_from_ak_string(action_text));
            search_selected_text_action->setVisible(true);
        } else {
            search_selected_text_action->setVisible(false);
        }

        m_page_context_menu->exec(view().map_point_to_global_position(content_position));
    };

    auto* open_link_in_new_tab_action = new QAction("Open Link in New &Tab", this);
    open_link_in_new_tab_action->setIcon(load_icon_from_uri("resource://icons/16x16/new-tab.png"sv));
    QObject::connect(open_link_in_new_tab_action, &QAction::triggered, this, [this]() {
        open_link_in_new_tab(m_link_context_menu_url);
    });

    m_link_context_menu_copy_url_action = new QAction("Copy &Link Address", this);
    m_link_context_menu_copy_url_action->setIcon(load_icon_from_uri("resource://icons/16x16/edit-copy.png"sv));
    QObject::connect(m_link_context_menu_copy_url_action, &QAction::triggered, this, [this]() {
        copy_link_url(m_link_context_menu_url);
    });

    m_link_context_menu = new QMenu("Link context menu", this);
    m_link_context_menu->addAction(open_link_in_new_tab_action);
    m_link_context_menu->addAction(m_link_context_menu_copy_url_action);
    m_link_context_menu->addSeparator();
    m_link_context_menu->addAction(&m_window->inspect_dom_node_action());

    view().on_link_context_menu_request = [this](auto const& url, Gfx::IntPoint content_position) {
        m_link_context_menu_url = url;

        switch (WebView::url_type(url)) {
        case WebView::URLType::Email:
            m_link_context_menu_copy_url_action->setText("Copy &Email Address");
            break;
        case WebView::URLType::Telephone:
            m_link_context_menu_copy_url_action->setText("Copy &Phone Number");
            break;
        case WebView::URLType::Other:
            m_link_context_menu_copy_url_action->setText("Copy &Link Address");
            break;
        }

        m_link_context_menu->exec(view().map_point_to_global_position(content_position));
    };

    auto* open_image_action = new QAction("&Open Image", this);
    open_image_action->setIcon(load_icon_from_uri("resource://icons/16x16/filetype-image.png"sv));
    QObject::connect(open_image_action, &QAction::triggered, this, [this]() {
        open_link(m_image_context_menu_url);
    });

    auto* open_image_in_new_tab_action = new QAction("&Open Image in New &Tab", this);
    open_image_in_new_tab_action->setIcon(load_icon_from_uri("resource://icons/16x16/new-tab.png"sv));
    QObject::connect(open_image_in_new_tab_action, &QAction::triggered, this, [this]() {
        open_link_in_new_tab(m_image_context_menu_url);
    });

    m_image_context_menu_copy_image_action = new QAction("&Copy Image", this);
    m_image_context_menu_copy_image_action->setIcon(load_icon_from_uri("resource://icons/16x16/edit-copy.png"sv));
    QObject::connect(m_image_context_menu_copy_image_action, &QAction::triggered, this, [this]() {
        if (!m_image_context_menu_bitmap.has_value())
            return;

        auto* bitmap = m_image_context_menu_bitmap.value().bitmap();
        if (bitmap == nullptr)
            return;

        auto data = Gfx::BMPWriter::encode(*bitmap);
        if (data.is_error())
            return;

        auto image = QImage::fromData(data.value().data(), data.value().size(), "BMP");
        if (image.isNull())
            return;

        auto* clipboard = QGuiApplication::clipboard();
        clipboard->setImage(image);
    });

    auto* copy_image_url_action = new QAction("Copy Image &URL", this);
    copy_image_url_action->setIcon(load_icon_from_uri("resource://icons/16x16/edit-copy.png"sv));
    QObject::connect(copy_image_url_action, &QAction::triggered, this, [this]() {
        copy_link_url(m_image_context_menu_url);
    });

    m_image_context_menu = new QMenu("Image context menu", this);
    m_image_context_menu->addAction(open_image_action);
    m_image_context_menu->addAction(open_image_in_new_tab_action);
    m_image_context_menu->addSeparator();
    m_image_context_menu->addAction(m_image_context_menu_copy_image_action);
    m_image_context_menu->addAction(copy_image_url_action);
    m_image_context_menu->addSeparator();
    m_image_context_menu->addAction(&m_window->inspect_dom_node_action());

    view().on_image_context_menu_request = [this](auto& image_url, Gfx::IntPoint content_position, Optional<Gfx::ShareableBitmap> const& shareable_bitmap) {
        m_image_context_menu_url = image_url;
        m_image_context_menu_bitmap = shareable_bitmap;
        m_image_context_menu_copy_image_action->setEnabled(shareable_bitmap.has_value());

        m_image_context_menu->exec(view().map_point_to_global_position(content_position));
    };

    m_media_context_menu_play_icon = load_icon_from_uri("resource://icons/16x16/play.png"sv);
    m_media_context_menu_pause_icon = load_icon_from_uri("resource://icons/16x16/pause.png"sv);
    m_media_context_menu_mute_icon = load_icon_from_uri("resource://icons/16x16/audio-volume-muted.png"sv);
    m_media_context_menu_unmute_icon = load_icon_from_uri("resource://icons/16x16/audio-volume-high.png"sv);

    m_media_context_menu_play_pause_action = new QAction("&Play", this);
    m_media_context_menu_play_pause_action->setIcon(m_media_context_menu_play_icon);
    QObject::connect(m_media_context_menu_play_pause_action, &QAction::triggered, this, [this]() {
        view().toggle_media_play_state();
    });

    m_media_context_menu_mute_unmute_action = new QAction("&Mute", this);
    m_media_context_menu_mute_unmute_action->setIcon(m_media_context_menu_mute_icon);
    QObject::connect(m_media_context_menu_mute_unmute_action, &QAction::triggered, this, [this]() {
        view().toggle_media_mute_state();
    });

    m_media_context_menu_controls_action = new QAction("Show &Controls", this);
    m_media_context_menu_controls_action->setCheckable(true);
    QObject::connect(m_media_context_menu_controls_action, &QAction::triggered, this, [this]() {
        view().toggle_media_controls_state();
    });

    m_media_context_menu_loop_action = new QAction("&Loop", this);
    m_media_context_menu_loop_action->setCheckable(true);
    QObject::connect(m_media_context_menu_loop_action, &QAction::triggered, this, [this]() {
        view().toggle_media_loop_state();
    });

    auto* open_audio_action = new QAction("&Open Audio", this);
    open_audio_action->setIcon(load_icon_from_uri("resource://icons/16x16/filetype-sound.png"sv));
    QObject::connect(open_audio_action, &QAction::triggered, this, [this]() {
        open_link(m_media_context_menu_url);
    });

    auto* open_audio_in_new_tab_action = new QAction("Open Audio in New &Tab", this);
    open_audio_in_new_tab_action->setIcon(load_icon_from_uri("resource://icons/16x16/new-tab.png"sv));
    QObject::connect(open_audio_in_new_tab_action, &QAction::triggered, this, [this]() {
        open_link_in_new_tab(m_media_context_menu_url);
    });

    auto* copy_audio_url_action = new QAction("Copy Audio &URL", this);
    copy_audio_url_action->setIcon(load_icon_from_uri("resource://icons/16x16/edit-copy.png"sv));
    QObject::connect(copy_audio_url_action, &QAction::triggered, this, [this]() {
        copy_link_url(m_media_context_menu_url);
    });

    m_audio_context_menu = new QMenu("Audio context menu", this);
    m_audio_context_menu->addAction(m_media_context_menu_play_pause_action);
    m_audio_context_menu->addAction(m_media_context_menu_mute_unmute_action);
    m_audio_context_menu->addAction(m_media_context_menu_controls_action);
    m_audio_context_menu->addAction(m_media_context_menu_loop_action);
    m_audio_context_menu->addSeparator();
    m_audio_context_menu->addAction(open_audio_action);
    m_audio_context_menu->addAction(open_audio_in_new_tab_action);
    m_audio_context_menu->addSeparator();
    m_audio_context_menu->addAction(copy_audio_url_action);
    m_audio_context_menu->addSeparator();
    m_audio_context_menu->addAction(&m_window->inspect_dom_node_action());

    auto* open_video_action = new QAction("&Open Video", this);
    open_video_action->setIcon(load_icon_from_uri("resource://icons/16x16/filetype-video.png"sv));
    QObject::connect(open_video_action, &QAction::triggered, this, [this]() {
        open_link(m_media_context_menu_url);
    });

    auto* open_video_in_new_tab_action = new QAction("Open Video in New &Tab", this);
    open_video_in_new_tab_action->setIcon(load_icon_from_uri("resource://icons/16x16/new-tab.png"sv));
    QObject::connect(open_video_in_new_tab_action, &QAction::triggered, this, [this]() {
        open_link_in_new_tab(m_media_context_menu_url);
    });

    auto* copy_video_url_action = new QAction("Copy Video &URL", this);
    copy_video_url_action->setIcon(load_icon_from_uri("resource://icons/16x16/edit-copy.png"sv));
    QObject::connect(copy_video_url_action, &QAction::triggered, this, [this]() {
        copy_link_url(m_media_context_menu_url);
    });

    m_video_context_menu = new QMenu("Video context menu", this);
    m_video_context_menu->addAction(m_media_context_menu_play_pause_action);
    m_video_context_menu->addAction(m_media_context_menu_mute_unmute_action);
    m_video_context_menu->addAction(m_media_context_menu_controls_action);
    m_video_context_menu->addAction(m_media_context_menu_loop_action);
    m_video_context_menu->addSeparator();
    m_video_context_menu->addAction(open_video_action);
    m_video_context_menu->addAction(open_video_in_new_tab_action);
    m_video_context_menu->addSeparator();
    m_video_context_menu->addAction(copy_video_url_action);
    m_video_context_menu->addSeparator();
    m_video_context_menu->addAction(&m_window->inspect_dom_node_action());

    view().on_media_context_menu_request = [this](Gfx::IntPoint content_position, Web::Page::MediaContextMenu const& menu) {
        m_media_context_menu_url = menu.media_url;

        if (menu.is_playing) {
            m_media_context_menu_play_pause_action->setIcon(m_media_context_menu_pause_icon);
            m_media_context_menu_play_pause_action->setText("&Pause");
        } else {
            m_media_context_menu_play_pause_action->setIcon(m_media_context_menu_play_icon);
            m_media_context_menu_play_pause_action->setText("&Play");
        }

        if (menu.is_muted) {
            m_media_context_menu_mute_unmute_action->setIcon(m_media_context_menu_unmute_icon);
            m_media_context_menu_mute_unmute_action->setText("Un&mute");
        } else {
            m_media_context_menu_mute_unmute_action->setIcon(m_media_context_menu_mute_icon);
            m_media_context_menu_mute_unmute_action->setText("&Mute");
        }

        m_media_context_menu_controls_action->setChecked(menu.has_user_agent_controls);
        m_media_context_menu_loop_action->setChecked(menu.is_looping);

        auto screen_position = view().map_point_to_global_position(content_position);
        if (menu.is_video)
            m_video_context_menu->exec(screen_position);
        else
            m_audio_context_menu->exec(screen_position);
    };
}

Tab::~Tab()
{
    close_sub_widgets();

    // Delete the InspectorWidget explicitly to ensure it is deleted before the WebContentView. Otherwise, Qt
    // can destroy these objects in any order, which may cause use-after-free in InspectorWidget's destructor.
    delete m_inspector_widget;
}

void Tab::update_reset_zoom_button()
{
    auto zoom_level = view().zoom_level();
    if (zoom_level != 1.0f) {
        auto zoom_level_text = MUST(String::formatted("{}%", round_to<int>(zoom_level * 100)));
        m_reset_zoom_button->setText(qstring_from_ak_string(zoom_level_text));
        m_reset_zoom_button_action->setVisible(true);
    } else {
        m_reset_zoom_button_action->setVisible(false);
    }
}

void Tab::focus_location_editor()
{
    m_location_edit->setFocus();
    m_location_edit->selectAll();
}

void Tab::navigate(URL::URL const& url)
{
    view().load(url);
}

void Tab::load_html(StringView html)
{
    view().load_html(html);
}

void Tab::back()
{
    view().traverse_the_history_by_delta(-1);
}

void Tab::forward()
{
    view().traverse_the_history_by_delta(1);
}

void Tab::reload()
{
    view().reload();
}

void Tab::open_link(URL::URL const& url)
{
    view().on_link_click(url, "", 0);
}

void Tab::open_link_in_new_tab(URL::URL const& url)
{
    view().on_link_click(url, "_blank", Web::UIEvents::Mod_Ctrl);
}

void Tab::copy_link_url(URL::URL const& url)
{
    auto* clipboard = QGuiApplication::clipboard();
    clipboard->setText(qstring_from_ak_string(WebView::url_text_to_copy(url)));
}

void Tab::location_edit_return_pressed()
{
    if (m_location_edit->text().isEmpty())
        return;
    navigate(m_location_edit->url());
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

void Tab::debug_request(ByteString const& request, ByteString const& argument)
{
    m_view->debug_request(request, argument);
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
    m_hover_label->resize(QFontMetrics(m_hover_label->font()).boundingRect(m_hover_label->text()).adjusted(-4, -2, 4, 2).size());

    auto hover_label_height = height() - m_hover_label->height();
    if (m_find_in_page->isVisible())
        hover_label_height -= m_find_in_page->height();

    if (m_hover_label->underMouse() && m_hover_label->x() == 0)
        m_hover_label->move(width() / 2 + (width() / 2 - m_hover_label->width()), hover_label_height);
    else
        m_hover_label->move(0, hover_label_height);

    m_hover_label->raise();
}

void Tab::update_navigation_buttons_state()
{
    if (m_window->current_tab() != this)
        return;

    m_window->go_back_action().setEnabled(m_can_navigate_back);
    m_window->go_forward_action().setEnabled(m_can_navigate_forward);
}

bool Tab::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange) {
        recreate_toolbar_icons();
        return QWidget::event(event);
    }

    return QWidget::event(event);
}

void Tab::recreate_toolbar_icons()
{
    m_window->go_back_action().setIcon(create_tvg_icon_with_theme_colors("back", palette()));
    m_window->go_forward_action().setIcon(create_tvg_icon_with_theme_colors("forward", palette()));
    m_window->reload_action().setIcon(create_tvg_icon_with_theme_colors("reload", palette()));
    m_window->new_tab_action().setIcon(create_tvg_icon_with_theme_colors("new_tab", palette()));
    m_hamburger_button->setIcon(create_tvg_icon_with_theme_colors("hamburger", palette()));
}

void Tab::recreate_inspector()
{
    if (m_inspector_widget)
        m_inspector_widget->deleteLater();

    m_inspector_widget = new InspectorWidget(this, view());

    QObject::connect(m_inspector_widget, &InspectorWidget::closed, [this] {
        m_inspector_widget->deleteLater();
        m_inspector_widget = nullptr;
    });
}

void Tab::show_inspector_window(InspectorTarget inspector_target)
{
    if (!m_inspector_widget)
        recreate_inspector();
    else
        m_inspector_widget->inspect();

    m_inspector_widget->show();
    m_inspector_widget->activateWindow();
    m_inspector_widget->raise();

    if (inspector_target == InspectorTarget::HoveredElement)
        m_inspector_widget->select_hovered_node();
    else
        m_inspector_widget->select_default_node();
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

void Tab::close_sub_widgets()
{
    auto close_widget_window = [](auto* widget) {
        if (widget)
            widget->close();
    };

    close_widget_window(m_inspector_widget);
}

void Tab::set_block_popups(bool enabled)
{
    debug_request("block-pop-ups", enabled ? "on" : "off");
}

void Tab::set_line_box_borders(bool enabled)
{
    debug_request("set-line-box-borders", enabled ? "on" : "off");
}

void Tab::set_same_origin_policy(bool enabled)
{
    debug_request("same-origin-policy", enabled ? "on" : "off");
}

void Tab::set_scripting(bool enabled)
{
    debug_request("scripting", enabled ? "on" : "off");
}

void Tab::set_user_agent_string(ByteString const& user_agent)
{
    debug_request("spoof-user-agent", user_agent);
    // Clear the cache to ensure requests are re-done with the new user agent.
    debug_request("clear-cache");
}

void Tab::set_navigator_compatibility_mode(ByteString const& compatibility_mode)
{
    debug_request("navigator-compatibility-mode", compatibility_mode);
}

void Tab::set_preferred_languages(ReadonlySpan<String> preferred_languages)
{
    m_view->set_preferred_languages(preferred_languages);
}

void Tab::set_enable_do_not_track(bool enable)
{
    m_view->set_enable_do_not_track(enable);
}

void Tab::set_enable_autoplay(bool enable)
{
    m_view->set_enable_autoplay(enable);
}

}
