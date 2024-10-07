/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Ladybird/HelperProcess.h>
#include <Ladybird/Qt/Application.h>
#include <Ladybird/Qt/Settings.h>
#include <Ladybird/Qt/StringUtils.h>
#include <Ladybird/Qt/TaskManagerWindow.h>
#include <Ladybird/Utilities.h>
#include <LibCore/ArgsParser.h>
#include <LibWebView/URL.h>
#include <QFileDialog>
#include <QFileOpenEvent>

namespace Ladybird {

Application::Application(Badge<WebView::Application>, Main::Arguments& arguments)
    : QApplication(arguments.argc, arguments.argv)
{
}

void Application::create_platform_options(WebView::ChromeOptions&, WebView::WebContentOptions& web_content_options)
{
    web_content_options.config_path = Settings::the()->directory();
}

Application::~Application()
{
    close_task_manager_window();
}

bool Application::event(QEvent* event)
{
    switch (event->type()) {
    case QEvent::FileOpen: {
        if (!on_open_file)
            break;

        auto const& open_event = *static_cast<QFileOpenEvent const*>(event);
        auto file = ak_string_from_qstring(open_event.file());

        if (auto file_url = WebView::sanitize_url(file); file_url.has_value())
            on_open_file(file_url.release_value());
        break;
    }
    default:
        break;
    }

    return QApplication::event(event);
}

static ErrorOr<NonnullRefPtr<ImageDecoderClient::Client>> launch_new_image_decoder()
{
    auto paths = TRY(get_paths_for_helper_process("ImageDecoder"sv));
    return launch_image_decoder_process(paths);
}

ErrorOr<void> Application::initialize_image_decoder()
{
    m_image_decoder_client = TRY(launch_new_image_decoder());

    m_image_decoder_client->on_death = [this] {
        m_image_decoder_client = nullptr;
        if (auto err = this->initialize_image_decoder(); err.is_error()) {
            dbgln("Failed to restart image decoder: {}", err.error());
            VERIFY_NOT_REACHED();
        }

        auto num_clients = WebView::WebContentClient::client_count();
        auto new_sockets = m_image_decoder_client->send_sync_but_allow_failure<Messages::ImageDecoderServer::ConnectNewClients>(num_clients);
        if (!new_sockets || new_sockets->sockets().size() == 0) {
            dbgln("Failed to connect {} new clients to ImageDecoder", num_clients);
            VERIFY_NOT_REACHED();
        }

        WebView::WebContentClient::for_each_client([sockets = new_sockets->take_sockets()](WebView::WebContentClient& client) mutable {
            client.async_connect_to_image_decoder(sockets.take_last());
            return IterationDecision::Continue;
        });
    };
    return {};
}

void Application::show_task_manager_window()
{
    if (!m_task_manager_window) {
        m_task_manager_window = new TaskManagerWindow(nullptr);
    }
    m_task_manager_window->show();
    m_task_manager_window->activateWindow();
    m_task_manager_window->raise();
}

void Application::close_task_manager_window()
{
    if (m_task_manager_window) {
        m_task_manager_window->close();
        delete m_task_manager_window;
        m_task_manager_window = nullptr;
    }
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

Optional<ByteString> Application::ask_user_for_download_folder() const
{
    auto path = QFileDialog::getExistingDirectory(nullptr, "Select download directory", QDir::homePath());
    if (path.isNull())
        return {};

    return ak_byte_string_from_qstring(path);
}

}
