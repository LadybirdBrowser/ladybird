/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/ArgsParser.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWebView/Application.h>
#include <LibWebView/URL.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

Application* Application::s_the = nullptr;

Application::Application()
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

void Application::initialize(Main::Arguments const& arguments, URL::URL new_tab_page_url)
{
    Vector<ByteString> raw_urls;
    Vector<ByteString> certificates;
    bool new_window = false;
    bool force_new_process = false;
    bool allow_popups = false;
    bool disable_sql_database = false;
    Optional<StringView> webdriver_content_ipc_path;
    bool enable_callgrind_profiling = false;
    bool debug_web_content = false;
    bool log_all_js_exceptions = false;
    bool enable_idl_tracing = false;
    bool enable_http_cache = false;
    bool expose_internals_object = false;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("The Ladybird web browser :^)");
    args_parser.add_positional_argument(raw_urls, "URLs to open", "url", Core::ArgsParser::Required::No);
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(new_window, "Force opening in a new window", "new-window", 'n');
    args_parser.add_option(force_new_process, "Force creation of new browser/chrome process", "force-new-process");
    args_parser.add_option(allow_popups, "Disable popup blocking by default", "allow-popups");
    args_parser.add_option(disable_sql_database, "Disable SQL database", "disable-sql-database");
    args_parser.add_option(webdriver_content_ipc_path, "Path to WebDriver IPC for WebContent", "webdriver-content-path", 0, "path", Core::ArgsParser::OptionHideMode::CommandLineAndMarkdown);
    args_parser.add_option(enable_callgrind_profiling, "Enable Callgrind profiling", "enable-callgrind-profiling", 'P');
    args_parser.add_option(debug_web_content, "Wait for debugger to attach to WebContent", "debug-web-content");
    args_parser.add_option(log_all_js_exceptions, "Log all JavaScript exceptions", "log-all-js-exceptions");
    args_parser.add_option(enable_idl_tracing, "Enable IDL tracing", "enable-idl-tracing");
    args_parser.add_option(enable_http_cache, "Enable HTTP cache", "enable-http-cache");
    args_parser.add_option(expose_internals_object, "Expose internals object", "expose-internals-object");

    create_platform_arguments(args_parser);
    args_parser.parse(arguments);

    m_chrome_options = {
        .urls = sanitize_urls(raw_urls, new_tab_page_url),
        .raw_urls = move(raw_urls),
        .new_tab_page_url = move(new_tab_page_url),
        .certificates = move(certificates),
        .new_window = new_window ? NewWindow::Yes : NewWindow::No,
        .force_new_process = force_new_process ? ForceNewProcess::Yes : ForceNewProcess::No,
        .allow_popups = allow_popups ? AllowPopups::Yes : AllowPopups::No,
        .disable_sql_database = disable_sql_database ? DisableSQLDatabase::Yes : DisableSQLDatabase::No,
    };

    if (webdriver_content_ipc_path.has_value())
        m_chrome_options.webdriver_content_ipc_path = *webdriver_content_ipc_path;

    m_web_content_options = {
        .command_line = MUST(String::join(' ', arguments.strings)),
        .executable_path = MUST(String::from_byte_string(MUST(Core::System::current_executable_path()))),
        .enable_callgrind_profiling = enable_callgrind_profiling ? EnableCallgrindProfiling::Yes : EnableCallgrindProfiling::No,
        .wait_for_debugger = debug_web_content ? WaitForDebugger::Yes : WaitForDebugger::No,
        .log_all_js_exceptions = log_all_js_exceptions ? LogAllJSExceptions::Yes : LogAllJSExceptions::No,
        .enable_idl_tracing = enable_idl_tracing ? EnableIDLTracing::Yes : EnableIDLTracing::No,
        .enable_http_cache = enable_http_cache ? EnableHTTPCache::Yes : EnableHTTPCache::No,
        .expose_internals_object = expose_internals_object ? ExposeInternalsObject::Yes : ExposeInternalsObject::No,
    };

    create_platform_options(m_chrome_options, m_web_content_options);
}

int Application::execute()
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
