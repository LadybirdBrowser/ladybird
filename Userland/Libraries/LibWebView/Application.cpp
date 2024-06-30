/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWebView/Application.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

Application* Application::s_the = nullptr;

Application::Application(int, char**)
{
    VERIFY(!s_the);
    s_the = this;

    m_process_manager.on_process_exited = [this](Process&& process) {
        process_did_exit(move(process));
    };
}

Application::~Application()
{
    s_the = nullptr;
}

int Application::exec()
{
    int ret = m_event_loop.exec();
    m_in_shutdown = true;
    return ret;
}

void Application::add_child_process(WebView::Process&& process)
{
    m_process_manager.add_process(move(process));
}

#if defined(AK_OS_MACH)
void Application::set_process_mach_port(pid_t pid, Core::MachPort&& port)
{
    m_process_manager.set_process_mach_port(pid, move(port));
}
#endif

Optional<Process&> Application::find_process(pid_t pid)
{
    return m_process_manager.find_process(pid);
}

void Application::update_process_statistics()
{
    m_process_manager.update_all_process_statistics();
}

String Application::generate_process_statistics_html()
{
    return m_process_manager.generate_html();
}

void Application::process_did_exit(Process&& process)
{
    if (m_in_shutdown)
        return;

    dbgln_if(WEBVIEW_PROCESS_DEBUG, "Process {} died, type: {}", process.pid(), process_name_from_type(process.type()));

    switch (process.type()) {
    case ProcessType::ImageDecoder:
        if (auto client = process.client<ImageDecoderClient::Client>(); client.has_value()) {
            dbgln_if(WEBVIEW_PROCESS_DEBUG, "Restart ImageDecoder process");
            if (auto on_death = move(client->on_death)) {
                on_death();
            }
        }
        break;
    case ProcessType::RequestServer:
        dbgln_if(WEBVIEW_PROCESS_DEBUG, "FIXME: Restart request server");
        break;
    case ProcessType::WebContent:
        if (auto client = process.client<WebContentClient>(); client.has_value()) {
            dbgln_if(WEBVIEW_PROCESS_DEBUG, "Restart WebContent process");
            if (auto on_web_content_process_crash = move(client->on_web_content_process_crash))
                on_web_content_process_crash();
        }
        break;
    case ProcessType::WebWorker:
        dbgln_if(WEBVIEW_PROCESS_DEBUG, "WebWorker {} died, not sure what to do.", process.pid());
        break;
    case ProcessType::Chrome:
        dbgln("Invalid process type to be dying: Chrome");
        VERIFY_NOT_REACHED();
    }
}

}
