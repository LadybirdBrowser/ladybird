/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/StandardPaths.h>
#include <LibRequests/RequestClient.h>
#include <LibWebView/EventLoop/EventLoopImplementationQt.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/QuarantineManagerDialog.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/StringUtils.h>

#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileOpenEvent>
#include <QMessageBox>
#include <QMimeData>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#    include <LibCore/TimeZoneWatcher.h>
#    include <QAbstractNativeEventFilter>
#endif

namespace Ladybird {

#if defined(AK_OS_WINDOWS)
class NativeWindowsTimeChangeEventFilter : public QAbstractNativeEventFilter {
public:
    NativeWindowsTimeChangeEventFilter(Core::TimeZoneWatcher& time_zone_watcher)
        : m_time_zone_watcher(time_zone_watcher)
    {
    }

    bool nativeEventFilter(QByteArray const& event_type, void* message, qintptr*) override
    {
        if (event_type == QByteArrayLiteral("windows_generic_MSG")) {
            auto msg = static_cast<MSG*>(message);
            if (msg->message == WM_TIMECHANGE) {
                m_time_zone_watcher.on_time_zone_changed();
            }
        }
        return false;
    }

private:
    Core::TimeZoneWatcher& m_time_zone_watcher;
};

#endif

class LadybirdQApplication : public QApplication {
public:
    explicit LadybirdQApplication(Main::Arguments& arguments)
        : QApplication(arguments.argc, arguments.argv)
    {
    }

    virtual bool event(QEvent* event) override
    {
        auto& application = static_cast<Application&>(WebView::Application::the());

#if defined(AK_OS_WINDOWS)
        static Optional<NativeWindowsTimeChangeEventFilter> time_change_event_filter {};
        if (auto time_zone_watcher = application.time_zone_watcher(); !time_change_event_filter.has_value() && time_zone_watcher.has_value()) {
            time_change_event_filter.emplace(time_zone_watcher.value());
            installNativeEventFilter(&time_change_event_filter.value());
        }
#endif

        switch (event->type()) {
        case QEvent::FileOpen: {
            if (!application.on_open_file)
                break;

            auto const& open_event = *static_cast<QFileOpenEvent const*>(event);
            auto file = ak_string_from_qstring(open_event.file());

            if (auto file_url = WebView::sanitize_url(file); file_url.has_value())
                application.on_open_file(file_url.release_value());
            break;
        }

        default:
            break;
        }

        return QApplication::event(event);
    }
};

Application::Application() = default;
Application::~Application() = default;

void Application::create_platform_options(WebView::BrowserOptions&, WebView::WebContentOptions& web_content_options)
{
    web_content_options.config_path = Settings::the()->directory();
}

NonnullOwnPtr<Core::EventLoop> Application::create_platform_event_loop()
{
    if (!browser_options().headless_mode.has_value()) {
        Core::EventLoopManager::install(*new WebView::EventLoopManagerQt);
        m_application = make<LadybirdQApplication>(arguments());
    }

    auto event_loop = WebView::Application::create_platform_event_loop();

    if (!browser_options().headless_mode.has_value())
        static_cast<WebView::EventLoopImplementationQt&>(event_loop->impl()).set_main_loop();

    return event_loop;
}

BrowserWindow& Application::new_window(Vector<URL::URL> const& initial_urls, BrowserWindow::IsPopupWindow is_popup_window, Tab* parent_tab, Optional<u64> page_index)
{
    auto* window = new BrowserWindow(initial_urls, is_popup_window, parent_tab, move(page_index));
    set_active_window(*window);
    window->show();
    if (initial_urls.is_empty()) {
        auto* tab = window->current_tab();
        if (tab) {
            tab->set_url_is_hidden(true);
            tab->focus_location_editor();
        }
    }
    window->activateWindow();
    window->raise();
    return *window;
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    if (auto* active_tab = this->active_tab())
        return active_tab->view();
    return {};
}

Optional<WebView::ViewImplementation&> Application::open_blank_new_tab(Web::HTML::ActivateTab activate_tab) const
{
    auto& tab = active_window().create_new_tab(activate_tab);
    return tab.view();
}

Optional<ByteString> Application::ask_user_for_download_folder() const
{
    auto path = QFileDialog::getExistingDirectory(nullptr, "Select download directory", QDir::homePath());
    if (path.isNull())
        return {};

    return ak_byte_string_from_qstring(path);
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    auto message = MUST(String::formatted("{} saved to: {}", download_name, path));

    QMessageBox dialog(active_tab());
    dialog.setWindowTitle("Ladybird");
    dialog.setIcon(QMessageBox::Information);
    dialog.setText(qstring_from_ak_string(message));
    dialog.addButton(QMessageBox::Ok);
    dialog.addButton(QMessageBox::Open)->setText("Open folder");

    if (dialog.exec() == QMessageBox::Open) {
        auto path_url = QUrl::fromLocalFile(qstring_from_ak_string(path.dirname()));
        QDesktopServices::openUrl(path_url);
    }
}

void Application::display_error_dialog(StringView error_message) const
{
    QMessageBox::warning(active_tab(), "Ladybird", qstring_from_ak_string(error_message));
}

void Application::on_quarantine_manager_requested() const
{
    dbgln("Application: Opening Quarantine Manager dialog");

    // Get quarantine directory and entries from RequestServer via IPC
    auto& request_client = WebView::Application::request_server_client();

    auto directory_path = request_client.get_quarantine_directory();
    auto entries_json_list = request_client.list_quarantine_entries();

    // Parse quarantine entries from JSON
    Vector<QuarantineManagerDialog::QuarantineEntry> entries;

    for (auto const& entry_json_str : entries_json_list) {
        auto json_result = JsonValue::from_string(entry_json_str);
        if (json_result.is_error()) {
            dbgln("Failed to parse quarantine entry JSON: {}", json_result.error());
            continue;
        }

        auto json = json_result.release_value();
        if (!json.is_object())
            continue;

        auto obj = json.as_object();

        QuarantineManagerDialog::QuarantineEntry entry;
        entry.quarantine_id = obj.get_string("quarantine_id"sv).value_or({}).to_byte_string();
        entry.filename = obj.get_string("filename"sv).value_or({}).to_byte_string();
        entry.original_url = obj.get_string("original_url"sv).value_or({}).to_byte_string();
        entry.detection_time = obj.get_string("detection_time"sv).value_or({}).to_byte_string();
        entry.sha256 = obj.get_string("sha256"sv).value_or({}).to_byte_string();
        entry.file_size = obj.get_u64("file_size"sv).value_or(0);

        // Parse rule names array
        auto rules_array = obj.get_array("rule_names"sv);
        if (rules_array.has_value()) {
            for (size_t i = 0; i < rules_array->size(); i++) {
                auto rule = rules_array->at(i);
                if (rule.is_string()) {
                    entry.rule_names.append(rule.as_string().to_byte_string());
                }
            }
        }

        entries.append(move(entry));
    }

    // Create and show the dialog
    auto* dialog = new QuarantineManagerDialog(m_active_window);
    dialog->set_quarantine_entries(entries);
    dialog->set_quarantine_directory(directory_path);

    // Connect signals for restore and delete operations
    QObject::connect(dialog, &QuarantineManagerDialog::restore_requested,
        [](ByteString quarantine_id) {
            dbgln("Restore requested for: {}", quarantine_id);

            // Get downloads directory
            auto downloads_dir = ByteString::formatted("{}/Downloads", Core::StandardPaths::home_directory());

            // Call RequestServer IPC to restore file
            auto& request_client = WebView::Application::request_server_client();
            auto success = request_client.restore_quarantine_file(quarantine_id, downloads_dir);

            if (success) {
                QMessageBox::information(nullptr, "Restore Complete",
                    QString("File restored to Downloads directory."));
            } else {
                QMessageBox::critical(nullptr, "Restore Failed",
                    QString("Failed to restore file from quarantine."));
            }
        });

    QObject::connect(dialog, &QuarantineManagerDialog::delete_requested,
        [](ByteString quarantine_id) {
            dbgln("Delete requested for: {}", quarantine_id);

            // Call RequestServer IPC to delete file
            auto& request_client = WebView::Application::request_server_client();
            auto success = request_client.delete_quarantine_file(quarantine_id);

            if (!success) {
                QMessageBox::critical(nullptr, "Delete Failed",
                    QString("Failed to delete file from quarantine."));
            }
        });

    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

Utf16String Application::clipboard_text() const
{
    if (browser_options().headless_mode.has_value())
        return WebView::Application::clipboard_text();

    auto const* clipboard = QGuiApplication::clipboard();
    return utf16_string_from_qstring(clipboard->text());
}

Vector<Web::Clipboard::SystemClipboardRepresentation> Application::clipboard_entries() const
{
    if (browser_options().headless_mode.has_value())
        return WebView::Application::clipboard_entries();

    Vector<Web::Clipboard::SystemClipboardRepresentation> representations;
    auto const* clipboard = QGuiApplication::clipboard();

    auto const* mime_data = clipboard->mimeData();
    if (!mime_data)
        return {};

    for (auto const& format : mime_data->formats()) {
        auto data = ak_byte_string_from_qbytearray(mime_data->data(format));
        auto mime_type = ak_string_from_qstring(format);

        representations.empend(move(data), move(mime_type));
    }

    return representations;
}

void Application::insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation entry)
{
    if (browser_options().headless_mode.has_value()) {
        WebView::Application::insert_clipboard_entry(move(entry));
        return;
    }

    auto* mime_data = new QMimeData();
    mime_data->setData(qstring_from_ak_string(entry.mime_type), qbytearray_from_ak_string(entry.data));

    auto* clipboard = QGuiApplication::clipboard();
    clipboard->setMimeData(mime_data);
}

void Application::on_devtools_enabled() const
{
    WebView::Application::on_devtools_enabled();

    if (m_active_window)
        m_active_window->on_devtools_enabled();
}

void Application::on_devtools_disabled() const
{
    WebView::Application::on_devtools_disabled();

    if (m_active_window)
        m_active_window->on_devtools_disabled();
}

}
