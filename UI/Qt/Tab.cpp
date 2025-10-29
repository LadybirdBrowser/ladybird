/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonParser.h>
#include <LibIPC/NetworkIdentity.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWebView/Application.h>
#include <Services/Sentinel/PolicyGraph.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/Menu.h>
#include <UI/Qt/NetworkAuditDialog.h>
#include <UI/Qt/ProxySettingsDialog.h>
#include <UI/Qt/SecurityAlertDialog.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/StringUtils.h>

#include <QColorDialog>
#include <QFileDialog>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMimeDatabase>
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

    auto view_initial_state = WebContentViewInitialState {
        .maximum_frames_per_second = window->refresh_rate(),
    };

    m_view = new WebContentView(this, parent_client, page_index, AK::move(view_initial_state));
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
    focus_location_editor_action->setShortcuts({ QKeySequence("Ctrl+L"), QKeySequence("Alt+D") });
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

    m_navigate_back_action = create_application_action(*this, view().navigate_back_action());
    m_navigate_forward_action = create_application_action(*this, view().navigate_forward_action());
    m_reload_action = create_application_action(*this, WebView::Application::the().reload_action());

    // Create Tor toggle button
    m_tor_toggle_action = new QAction(this);
    m_tor_toggle_action->setCheckable(true);
    m_tor_toggle_action->setChecked(false);
    m_tor_toggle_action->setText("Tor");  // Show "Tor" text on button
    m_tor_toggle_action->setToolTip("Enable Tor for this tab");
    QObject::connect(m_tor_toggle_action, &QAction::triggered, this, [this](bool checked) {
        if (checked) {
            // Check if Tor is available BEFORE enabling
            if (!IPC::TorAvailability::is_tor_running()) {
                // Tor not available - show error and revert toggle
                QMessageBox::warning(this, "Tor Not Available",
                    "Cannot enable Tor: The Tor service is not running.\n\n"
                    "Please start Tor first:\n"
                    "  Linux:   sudo systemctl start tor\n"
                    "  macOS:   brew services start tor\n"
                    "  Windows: Start Tor Browser or tor.exe\n\n"
                    "Need help? Visit: https://www.torproject.org/download/");

                // Revert toggle state
                m_tor_toggle_action->setChecked(false);
                m_tor_enabled = false;
                return;
            }

            // Tor is available - proceed with enabling
            m_tor_enabled = true;
            dbgln("Tab: Enabling Tor for page_id {}", view().page_id());
            m_tor_toggle_action->setToolTip("Disable Tor for this tab (currently using Tor)");
            // Apply green border to location edit to indicate Tor is active
            m_location_edit->setStyleSheet("QLineEdit { border: 2px solid #00C851; }");
            view().client().async_enable_tor(view().page_id(), {});
        } else {
            // Disable Tor for this tab
            m_tor_enabled = false;
            dbgln("Tab: Disabling Tor for page_id {}", view().page_id());
            m_tor_toggle_action->setToolTip("Enable Tor for this tab");
            // Remove green border when Tor is disabled
            m_location_edit->setStyleSheet("");
            view().client().async_disable_tor(view().page_id());
        }
    });

    // Create VPN toggle button
    m_vpn_toggle_action = new QAction(this);
    m_vpn_toggle_action->setCheckable(true);
    m_vpn_toggle_action->setChecked(false);
    m_vpn_toggle_action->setText("VPN");
    m_vpn_toggle_action->setToolTip("Enable VPN/Proxy for this tab");
    QObject::connect(m_vpn_toggle_action, &QAction::triggered, this, [this](bool checked) {
        if (checked) {
            // Check if proxy is configured
            if (!m_proxy_config.has_value()) {
                // No proxy configured - open settings dialog
                open_proxy_settings_dialog();

                // Revert toggle if dialog was cancelled
                if (!m_proxy_config.has_value()) {
                    m_vpn_toggle_action->setChecked(false);
                    return;
                }
            }

            // Apply proxy configuration
            m_vpn_enabled = true;
            dbgln("Tab: Enabling VPN for page_id {}", view().page_id());
            m_vpn_toggle_action->setToolTip("Disable VPN for this tab (currently using proxy)");

            // Apply border color based on Tor+VPN state
            if (m_tor_enabled) {
                // Both Tor + VPN active - purple border
                m_location_edit->setStyleSheet("QLineEdit { border: 2px solid #9C27B0; }");
            } else {
                // VPN only - blue border
                m_location_edit->setStyleSheet("QLineEdit { border: 2px solid #2196F3; }");
            }

            // Send IPC message to apply proxy
            auto const& config = *m_proxy_config;
            ByteString proxy_type_str;
            switch (config.type) {
            case IPC::ProxyType::SOCKS5H:
                proxy_type_str = "SOCKS5H";
                break;
            case IPC::ProxyType::SOCKS5:
                proxy_type_str = "SOCKS5";
                break;
            case IPC::ProxyType::HTTP:
                proxy_type_str = "HTTP";
                break;
            case IPC::ProxyType::HTTPS:
                proxy_type_str = "HTTPS";
                break;
            default:
                proxy_type_str = "SOCKS5H";
                break;
            }
            view().client().async_set_proxy(view().page_id(), config.host, config.port, proxy_type_str, config.username, config.password);
        } else {
            // Disable VPN
            m_vpn_enabled = false;
            dbgln("Tab: Disabling VPN for page_id {}", view().page_id());
            m_vpn_toggle_action->setToolTip("Enable VPN/Proxy for this tab");

            // Update border color
            if (m_tor_enabled) {
                // Tor still active - revert to green border
                m_location_edit->setStyleSheet("QLineEdit { border: 2px solid #00C851; }");
            } else {
                // Neither active - remove border
                m_location_edit->setStyleSheet("");
            }

            // Send IPC message to clear proxy
            view().client().async_clear_proxy(view().page_id());
        }
    });

    // Create Network Audit button
    m_network_audit_action = new QAction(this);
    m_network_audit_action->setIcon(load_icon_from_uri("resource://icons/16x16/network.png"sv));
    m_network_audit_action->setText("Network Activity");
    m_network_audit_action->setToolTip("View network activity audit log");
    QObject::connect(m_network_audit_action, &QAction::triggered, this, &Tab::open_network_audit_dialog);

    recreate_toolbar_icons();

    m_favicon = default_favicon();

    m_page_context_menu = create_context_menu(*this, view(), view().page_context_menu());
    m_link_context_menu = create_context_menu(*this, view(), view().link_context_menu());
    m_image_context_menu = create_context_menu(*this, view(), view().image_context_menu());
    m_media_context_menu = create_context_menu(*this, view(), view().media_context_menu());

    m_toolbar->addAction(m_navigate_back_action);
    m_toolbar->addAction(m_navigate_forward_action);
    m_toolbar->addAction(m_reload_action);
    m_toolbar->addAction(m_tor_toggle_action);  // Add Tor toggle button
    m_toolbar->addAction(m_vpn_toggle_action);  // Add VPN toggle button
    m_toolbar->addAction(m_network_audit_action);  // Add Network Audit button
    m_toolbar->addWidget(m_location_edit);
    m_toolbar->addAction(create_application_action(*m_toolbar, view().reset_zoom_action()));
    m_hamburger_button_action = m_toolbar->addWidget(m_hamburger_button);

    m_toolbar->setIconSize({ 16, 16 });
    m_toolbar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    // This is a little awkward, but without this Qt shrinks the button to the size of the icon.
    // Note: toolButtonStyle="0" -> ToolButtonIconOnly.
    m_toolbar->setStyleSheet("QToolButton[toolButtonStyle=\"0\"]{width:24px;height:24px}");

    m_hamburger_button_action->setVisible(!Settings::the()->show_menubar());

    QObject::connect(Settings::the(), &Settings::show_menubar_changed, this, [this](bool show_menubar) {
        m_hamburger_button_action->setVisible(!show_menubar);
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

    view().on_load_start = [this](URL::URL const& url, bool) {
        auto url_serialized = qstring_from_ak_string(url.serialize());

        m_title = url_serialized;
        emit title_changed(tab_index(), url_serialized);

        m_favicon = default_favicon();
        emit favicon_changed(tab_index(), m_favicon);

        m_location_edit->set_url(url);
        m_location_edit->setCursorPosition(0);
    };

    view().on_url_change = [this](auto const& url) {
        m_location_edit->set_url(url);
    };

    QObject::connect(m_location_edit, &QLineEdit::returnPressed, this, &Tab::location_edit_return_pressed);

    view().on_title_change = [this](auto const& title) {
        m_title = qstring_from_utf16_string(title);
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

    // Security alert callback (Phase 3 Day 17-18)
    view().on_security_alert = [this](ByteString const& alert_json, i32 request_id) {
        // Parse the alert JSON to extract threat details
        auto json_result = JsonValue::from_string(alert_json);
        if (json_result.is_error()) {
            dbgln("Failed to parse security alert JSON: {}", alert_json);
            return;
        }

        auto alert_obj = json_result.value().as_object();

        // Extract basic threat information
        auto url = alert_obj.get_string("url"sv).value_or(""_string);
        auto filename = alert_obj.get_string("filename"sv).value_or("Unknown file"_string);
        auto file_hash = alert_obj.get_string("file_hash"sv).value_or(""_string);
        auto mime_type = alert_obj.get_string("mime_type"sv).value_or("application/octet-stream"_string);
        auto file_size = alert_obj.get_integer<qint64>("file_size"sv).value_or(0);

        // Extract first matched rule details
        QString rule_name = "Unknown";
        QString severity = "unknown";
        QString description = "No description available";

        if (auto matched_rules = alert_obj.get_array("matched_rules"sv); matched_rules.has_value() && !matched_rules->is_empty()) {
            auto first_rule = matched_rules->at(0).as_object();
            rule_name = QString::fromUtf8(first_rule.get_string("rule_name"sv).value_or("Unknown"_string).to_byte_string().characters());
            severity = QString::fromUtf8(first_rule.get_string("severity"sv).value_or("unknown"_string).to_byte_string().characters());
            description = QString::fromUtf8(first_rule.get_string("description"sv).value_or("No description"_string).to_byte_string().characters());
        }

        // Create SecurityAlertDialog
        SecurityAlertDialog::ThreatDetails details {
            .url = QString::fromUtf8(url.to_byte_string().characters()),
            .filename = QString::fromUtf8(filename.to_byte_string().characters()),
            .rule_name = rule_name,
            .severity = severity,
            .description = description,
            .file_hash = QString::fromUtf8(file_hash.to_byte_string().characters()),
            .mime_type = QString::fromUtf8(mime_type.to_byte_string().characters()),
            .file_size = file_size
        };

        m_dialog = new SecurityAlertDialog(details, &view());
        auto* security_dialog = qobject_cast<SecurityAlertDialog*>(m_dialog.data());

        QObject::connect(security_dialog, &SecurityAlertDialog::userDecided, this, [security_dialog, alert_obj, request_id](SecurityAlertDialog::UserDecision decision) {
            // TODO Phase 3 Day 19: Send enforcement decision via IPC
            // For now, just log the decision
            QString decision_str;
            switch (decision) {
            case SecurityAlertDialog::UserDecision::Block:
                decision_str = "block";
                break;
            case SecurityAlertDialog::UserDecision::AllowOnce:
                decision_str = "allow";
                break;
            case SecurityAlertDialog::UserDecision::AlwaysAllow:
                decision_str = "allow";
                break;
            }

            dbgln("Tab: User security decision for request {}: {} (remember: {})",
                  request_id, decision_str.toUtf8().data(), security_dialog->should_remember());

            // Phase 3 Day 19: Create policy in PolicyGraph if remember is checked
            if (security_dialog->should_remember()) {
                // Only create policies for Block and AlwaysAllow (not AllowOnce)
                if (decision == SecurityAlertDialog::UserDecision::Block ||
                    decision == SecurityAlertDialog::UserDecision::AlwaysAllow) {

                    // Extract URL and rule from alert JSON
                    auto url = alert_obj.get_string("url"sv).value_or(""_string);
                    auto url_pattern = ak_string_from_qstring(QString::fromUtf8(url.to_byte_string().characters()));

                    // Get first matched rule name
                    String rule_name = "Unknown"_string;
                    if (auto matched_rules = alert_obj.get_array("matched_rules"sv); matched_rules.has_value() && !matched_rules->is_empty()) {
                        auto first_rule = matched_rules->at(0).as_object();
                        rule_name = first_rule.get_string("rule_name"sv).value_or("Unknown"_string);
                    }

                    // Determine PolicyGraph action
                    Sentinel::PolicyGraph::PolicyAction action;
                    if (decision == SecurityAlertDialog::UserDecision::Block) {
                        action = Sentinel::PolicyGraph::PolicyAction::Block;
                    } else {
                        action = Sentinel::PolicyGraph::PolicyAction::Allow;
                    }

                    // Create policy in PolicyGraph
                    Sentinel::PolicyGraph::Policy policy {
                        .rule_name = rule_name,
                        .url_pattern = url_pattern,
                        .file_hash = {},
                        .mime_type = {},
                        .action = action,
                        .created_at = UnixDateTime::now(),
                        .created_by = "UI"_string,
                        .expires_at = {},
                        .last_hit = {}
                    };

                    // Get PolicyGraph instance and create policy
                    auto pg_result = Sentinel::PolicyGraph::create("/tmp/sentinel");
                    if (pg_result.is_error()) {
                        dbgln("Failed to access PolicyGraph: {}", pg_result.error());
                    } else {
                        auto& policy_graph = pg_result.value();
                        auto policy_result = policy_graph.create_policy(policy);

                        if (policy_result.is_error()) {
                            dbgln("Failed to create policy: {}", policy_result.error());
                        } else {
                            dbgln("Created policy: {} {} for {}",
                                action == Sentinel::PolicyGraph::PolicyAction::Allow ? "Allow" : "Block",
                                rule_name.to_byte_string().characters(), url_pattern.to_byte_string().characters());
                        }
                    }
                }
            }

            // TODO Phase 3 Day 19: Call view().client().async_enforce_security_policy(request_id, decision_str);
        });

        QObject::connect(m_dialog, &QDialog::finished, this, [this]() {
            m_dialog = nullptr;
        });

        m_dialog->open();
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
    m_context_menu->addAction(create_application_action(*this, WebView::Application::the().reload_action()));
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
    m_navigate_back_action->setIcon(create_tvg_icon_with_theme_colors("back", palette()));
    m_navigate_forward_action->setIcon(create_tvg_icon_with_theme_colors("forward", palette()));
    m_reload_action->setIcon(create_tvg_icon_with_theme_colors("reload", palette()));
    m_window->new_tab_action().setIcon(create_tvg_icon_with_theme_colors("new_tab", palette()));
    m_hamburger_button->setIcon(create_tvg_icon_with_theme_colors("hamburger", palette()));
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

void Tab::open_proxy_settings_dialog()
{
    // Create proxy settings dialog
    auto* dialog = new ProxySettingsDialog(this);

    // If we already have a proxy config, pre-populate the dialog
    if (m_proxy_config.has_value()) {
        dialog->set_proxy_config(*m_proxy_config);
    }

    // Show dialog and wait for result
    if (dialog->exec() == QDialog::Accepted) {
        // User clicked Save - get the proxy configuration
        m_proxy_config = dialog->get_proxy_config();
        dbgln("Tab: Proxy configuration saved for page_id {}", view().page_id());
    } else {
        // User clicked Cancel or closed dialog
        dbgln("Tab: Proxy configuration cancelled");
    }

    delete dialog;
}

void Tab::open_network_audit_dialog()
{
    // Get audit data via IPC
    auto response = view().client().get_network_audit(view().page_id());

    // Create and show dialog
    auto* dialog = new NetworkAuditDialog(this);
    dialog->set_audit_data(response.audit_entries(), response.total_bytes_sent(), response.total_bytes_received());
    dialog->exec();

    delete dialog;
}

}
