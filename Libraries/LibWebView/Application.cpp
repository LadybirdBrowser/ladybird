/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Environment.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/TimeZoneWatcher.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/Database.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/URL.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

Application* Application::s_the = nullptr;

Application::Application()
{
    VERIFY(!s_the);
    s_the = this;

    // No need to monitor the system time zone if the TZ environment variable is set, as it overrides system preferences.
    if (!Core::Environment::has("TZ"sv)) {
        if (auto time_zone_watcher = Core::TimeZoneWatcher::create(); time_zone_watcher.is_error()) {
            warnln("Unable to monitor system time zone: {}", time_zone_watcher.error());
        } else {
            m_time_zone_watcher = time_zone_watcher.release_value();

            m_time_zone_watcher->on_time_zone_changed = []() {
                WebContentClient::for_each_client([&](WebView::WebContentClient& client) {
                    client.async_system_time_zone_changed();
                    return IterationDecision::Continue;
                });
            };
        }
    }

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
    // Increase the open file limit, as the default limits on Linux cause us to run out of file descriptors with around 15 tabs open.
    if (auto result = Core::System::set_resource_limits(RLIMIT_NOFILE, 8192); result.is_error())
        warnln("Unable to increase open file limit: {}", result.error());

    Vector<ByteString> raw_urls;
    Vector<ByteString> certificates;
    bool new_window = false;
    bool force_new_process = false;
    bool allow_popups = false;
    bool disable_scripting = false;
    bool disable_sql_database = false;
    Optional<u16> devtools_port;
    Optional<StringView> debug_process;
    Optional<StringView> profile_process;
    Optional<StringView> webdriver_content_ipc_path;
    Optional<StringView> user_agent_preset;
    Optional<StringView> dns_server_address;
    Optional<u16> dns_server_port;
    bool use_dns_over_tls = false;
    bool log_all_js_exceptions = false;
    bool enable_idl_tracing = false;
    bool enable_http_cache = false;
    bool enable_autoplay = false;
    bool expose_internals_object = false;
    bool force_cpu_painting = false;
    bool force_fontconfig = false;
    bool collect_garbage_on_every_allocation = false;
    bool disable_scrollbar_painting = false;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("The Ladybird web browser :^)");
    args_parser.add_positional_argument(raw_urls, "URLs to open", "url", Core::ArgsParser::Required::No);
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(new_window, "Force opening in a new window", "new-window", 'n');
    args_parser.add_option(force_new_process, "Force creation of new browser/chrome process", "force-new-process");
    args_parser.add_option(allow_popups, "Disable popup blocking by default", "allow-popups");
    args_parser.add_option(disable_scripting, "Disable scripting by default", "disable-scripting");
    args_parser.add_option(disable_sql_database, "Disable SQL database", "disable-sql-database");
    args_parser.add_option(debug_process, "Wait for a debugger to attach to the given process name (WebContent, RequestServer, etc.)", "debug-process", 0, "process-name");
    args_parser.add_option(profile_process, "Enable callgrind profiling of the given process name (WebContent, RequestServer, etc.)", "profile-process", 0, "process-name");
    args_parser.add_option(webdriver_content_ipc_path, "Path to WebDriver IPC for WebContent", "webdriver-content-path", 0, "path", Core::ArgsParser::OptionHideMode::CommandLineAndMarkdown);
    args_parser.add_option(devtools_port, "Set the Firefox DevTools port (EXPERIMENTAL)", "devtools", 0, "port");
    args_parser.add_option(log_all_js_exceptions, "Log all JavaScript exceptions", "log-all-js-exceptions");
    args_parser.add_option(enable_idl_tracing, "Enable IDL tracing", "enable-idl-tracing");
    args_parser.add_option(enable_http_cache, "Enable HTTP cache", "enable-http-cache");
    args_parser.add_option(enable_autoplay, "Enable multimedia autoplay", "enable-autoplay");
    args_parser.add_option(expose_internals_object, "Expose internals object", "expose-internals-object");
    args_parser.add_option(force_cpu_painting, "Force CPU painting", "force-cpu-painting");
    args_parser.add_option(force_fontconfig, "Force using fontconfig for font loading", "force-fontconfig");
    args_parser.add_option(collect_garbage_on_every_allocation, "Collect garbage after every JS heap allocation", "collect-garbage-on-every-allocation", 'g');
    args_parser.add_option(disable_scrollbar_painting, "Don't paint horizontal or vertical scrollbars on the main viewport", "disable-scrollbar-painting");
    args_parser.add_option(dns_server_address, "Set the DNS server address", "dns-server", 0, "host|address");
    args_parser.add_option(dns_server_port, "Set the DNS server port", "dns-port", 0, "port (default: 53 or 853 if --dot)");
    args_parser.add_option(use_dns_over_tls, "Use DNS over TLS", "dot");
    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Name of the User-Agent preset to use in place of the default User-Agent",
        .long_name = "user-agent-preset",
        .value_name = "name",
        .accept_value = [&](StringView value) {
            user_agent_preset = normalize_user_agent_name(value);
            return user_agent_preset.has_value();
        },
    });

    create_platform_arguments(args_parser);
    args_parser.parse(arguments);

    // Our persisted SQL storage assumes it runs in a singleton process. If we have multiple UI processes accessing
    // the same underlying database, one of them is likely to fail.
    if (force_new_process)
        disable_sql_database = true;

    if (!dns_server_port.has_value())
        dns_server_port = use_dns_over_tls ? 853 : 53;

    Optional<ProcessType> debug_process_type;
    Optional<ProcessType> profile_process_type;

    if (debug_process.has_value())
        debug_process_type = process_type_from_name(*debug_process);
    if (profile_process.has_value())
        profile_process_type = process_type_from_name(*profile_process);

    m_chrome_options = {
        .urls = sanitize_urls(raw_urls, new_tab_page_url),
        .raw_urls = move(raw_urls),
        .new_tab_page_url = move(new_tab_page_url),
        .certificates = move(certificates),
        .new_window = new_window ? NewWindow::Yes : NewWindow::No,
        .force_new_process = force_new_process ? ForceNewProcess::Yes : ForceNewProcess::No,
        .allow_popups = allow_popups ? AllowPopups::Yes : AllowPopups::No,
        .disable_scripting = disable_scripting ? DisableScripting::Yes : DisableScripting::No,
        .disable_sql_database = disable_sql_database ? DisableSQLDatabase::Yes : DisableSQLDatabase::No,
        .debug_helper_process = move(debug_process_type),
        .profile_helper_process = move(profile_process_type),
        .dns_settings = (dns_server_address.has_value()
                ? (use_dns_over_tls
                          ? DNSSettings(DNSOverTLS(dns_server_address.release_value(), *dns_server_port))
                          : DNSSettings(DNSOverUDP(dns_server_address.release_value(), *dns_server_port)))
                : SystemDNS {}),
        .devtools_port = devtools_port,
    };

    if (webdriver_content_ipc_path.has_value())
        m_chrome_options.webdriver_content_ipc_path = *webdriver_content_ipc_path;

    m_web_content_options = {
        .command_line = MUST(String::join(' ', arguments.strings)),
        .executable_path = MUST(String::from_byte_string(MUST(Core::System::current_executable_path()))),
        .user_agent_preset = move(user_agent_preset),
        .log_all_js_exceptions = log_all_js_exceptions ? LogAllJSExceptions::Yes : LogAllJSExceptions::No,
        .enable_idl_tracing = enable_idl_tracing ? EnableIDLTracing::Yes : EnableIDLTracing::No,
        .enable_http_cache = enable_http_cache ? EnableHTTPCache::Yes : EnableHTTPCache::No,
        .expose_internals_object = expose_internals_object ? ExposeInternalsObject::Yes : ExposeInternalsObject::No,
        .force_cpu_painting = force_cpu_painting ? ForceCPUPainting::Yes : ForceCPUPainting::No,
        .force_fontconfig = force_fontconfig ? ForceFontconfig::Yes : ForceFontconfig::No,
        .enable_autoplay = enable_autoplay ? EnableAutoplay::Yes : EnableAutoplay::No,
        .collect_garbage_on_every_allocation = collect_garbage_on_every_allocation ? CollectGarbageOnEveryAllocation::Yes : CollectGarbageOnEveryAllocation::No,
        .paint_viewport_scrollbars = disable_scrollbar_painting ? PaintViewportScrollbars::No : PaintViewportScrollbars::Yes,
    };

    create_platform_options(m_chrome_options, m_web_content_options);

    if (m_chrome_options.disable_sql_database == DisableSQLDatabase::No) {
        m_database = Database::create().release_value_but_fixme_should_propagate_errors();
        m_cookie_jar = CookieJar::create(*m_database).release_value_but_fixme_should_propagate_errors();
    } else {
        m_cookie_jar = CookieJar::create();
    }
}

ErrorOr<void> Application::launch_services()
{
    TRY(launch_request_server());
    TRY(launch_image_decoder_server());
    TRY(launch_devtools_server());
    return {};
}

ErrorOr<void> Application::launch_request_server()
{
    // FIXME: Create an abstraction to re-spawn the RequestServer and re-hook up its client hooks to each tab on crash
    m_request_server_client = TRY(launch_request_server_process());
    return {};
}

ErrorOr<void> Application::launch_image_decoder_server()
{
    m_image_decoder_client = TRY(launch_image_decoder_process());

    m_image_decoder_client->on_death = [this]() {
        m_image_decoder_client = nullptr;

        if (auto result = launch_image_decoder_server(); result.is_error()) {
            dbgln("Failed to restart image decoder: {}", result.error());
            VERIFY_NOT_REACHED();
        }

        auto client_count = WebContentClient::client_count();
        auto new_sockets = m_image_decoder_client->send_sync_but_allow_failure<Messages::ImageDecoderServer::ConnectNewClients>(client_count);
        if (!new_sockets || new_sockets->sockets().is_empty()) {
            dbgln("Failed to connect {} new clients to ImageDecoder", client_count);
            VERIFY_NOT_REACHED();
        }

        WebContentClient::for_each_client([sockets = new_sockets->take_sockets()](WebContentClient& client) mutable {
            client.async_connect_to_image_decoder(sockets.take_last());
            return IterationDecision::Continue;
        });
    };

    return {};
}

ErrorOr<void> Application::launch_devtools_server()
{
    if (m_chrome_options.devtools_port.has_value())
        m_devtools = TRY(DevTools::DevToolsServer::create(*this, *m_chrome_options.devtools_port));
    return {};
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

ErrorOr<LexicalPath> Application::path_for_downloaded_file(StringView file) const
{
    auto downloads_directory = Core::StandardPaths::downloads_directory();

    if (!FileSystem::is_directory(downloads_directory)) {
        auto maybe_downloads_directory = ask_user_for_download_folder();
        if (!maybe_downloads_directory.has_value())
            return Error::from_errno(ECANCELED);

        downloads_directory = maybe_downloads_directory.release_value();
    }

    if (!FileSystem::is_directory(downloads_directory))
        return Error::from_errno(ENOENT);

    return LexicalPath::join(downloads_directory, file);
}

void Application::refresh_tab_list()
{
    if (!m_devtools)
        return;
    m_devtools->refresh_tab_list();
}

Vector<DevTools::TabDescription> Application::tab_list() const
{
    Vector<DevTools::TabDescription> tabs;

    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        tabs.empend(view.view_id(), MUST(String::from_byte_string(view.title())), view.url().to_string());
        return IterationDecision::Continue;
    });

    return tabs;
}

Vector<DevTools::CSSProperty> Application::css_property_list() const
{
    Vector<DevTools::CSSProperty> property_list;

    for (auto i = to_underlying(Web::CSS::first_property_id); i <= to_underlying(Web::CSS::last_property_id); ++i) {
        auto property_id = static_cast<Web::CSS::PropertyID>(i);

        DevTools::CSSProperty property;
        property.name = Web::CSS::string_from_property_id(property_id).to_string();
        property.is_inherited = Web::CSS::is_inherited_property(property_id);
        property_list.append(move(property));
    }

    return property_list;
}

void Application::inspect_tab(DevTools::TabDescription const& description, OnTabInspectionComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->on_received_dom_tree = [&view = *view, on_complete = move(on_complete)](JsonObject dom_tree) {
        view.on_received_dom_tree = nullptr;
        on_complete(move(dom_tree));
    };

    view->inspect_dom_tree();
}

void Application::inspect_dom_node(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, Optional<Web::CSS::Selector::PseudoElement::Type> pseudo_element, OnDOMNodeInspectionComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->on_received_dom_node_properties = [&view = *view, on_complete = move(on_complete)](ViewImplementation::DOMNodeProperties properties) {
        view.on_received_dom_node_properties = nullptr;
        on_complete(move(properties));
    };

    view->inspect_dom_node(node_id, pseudo_element);
}

void Application::clear_inspected_dom_node(DevTools::TabDescription const& description) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->clear_inspected_dom_node();
}

void Application::highlight_dom_node(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, Optional<Web::CSS::Selector::PseudoElement::Type> pseudo_element) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->highlight_dom_node(node_id, pseudo_element);
}

void Application::clear_highlighted_dom_node(DevTools::TabDescription const& description) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->clear_highlighted_dom_node();
}

}
