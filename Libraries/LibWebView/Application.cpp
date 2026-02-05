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
#include <LibDatabase/Database.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/Loader/UserAgent.h>
#include <LibWebView/Application.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/Menu.h>
#include <LibWebView/URL.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>

#if defined(AK_OS_MACOS)
#    include <LibWebView/MachPortServer.h>
#endif

namespace WebView {

Application* Application::s_the = nullptr;

struct ApplicationSettingsObserver : public SettingsObserver {
    virtual void browsing_data_settings_changed() override
    {
        auto const& browsing_data_settings = Application::settings().browsing_data_settings();
        Application::request_server_client().async_set_disk_cache_settings(browsing_data_settings.disk_cache_settings);
    }

    virtual void dns_settings_changed() override
    {
        Application::settings().dns_settings().visit(
            [](SystemDNS) {
                Application::request_server_client().async_set_use_system_dns();
            },
            [](DNSOverTLS const& dns_over_tls) {
                dbgln("Setting DNS server to {}:{} with TLS ({} local dnssec)", dns_over_tls.server_address, dns_over_tls.port, dns_over_tls.validate_dnssec_locally ? "with" : "without");
                Application::request_server_client().async_set_dns_server(dns_over_tls.server_address, dns_over_tls.port, true, dns_over_tls.validate_dnssec_locally);
            },
            [](DNSOverUDP const& dns_over_udp) {
                dbgln("Setting DNS server to {}:{} ({} local dnssec)", dns_over_udp.server_address, dns_over_udp.port, dns_over_udp.validate_dnssec_locally ? "with" : "without");
                Application::request_server_client().async_set_dns_server(dns_over_udp.server_address, dns_over_udp.port, false, dns_over_udp.validate_dnssec_locally);
            });
    }
};

Application::Application(Optional<ByteString> ladybird_binary_path)
    : m_settings(Settings::create({}))
{
    VERIFY(!s_the);
    s_the = this;

    platform_init(move(ladybird_binary_path));
}

Application::~Application()
{
    // Explicitly delete the settings observer first, as the observer destructor will refer to Application::the().
    m_settings_observer.clear();

    s_the = nullptr;
}

ErrorOr<void> Application::initialize(Main::Arguments const& arguments)
{
    TRY(handle_attached_debugger());
    m_arguments = arguments;

#if !defined(AK_OS_WINDOWS)
    // Increase the open file limit, as the default limits on Linux cause us to run out of file descriptors with around 15 tabs open.
    if (auto result = Core::System::set_resource_limits(RLIMIT_NOFILE, 8192); result.is_error())
        warnln("Unable to increase open file limit: {}", result.error());
#endif

#if defined(AK_OS_MACOS)
    m_mach_port_server = make<MachPortServer>();
    set_mach_server_name(m_mach_port_server->server_port_name());

    m_mach_port_server->on_receive_child_mach_port = [this](auto pid, auto port) {
        set_process_mach_port(pid, move(port));
    };
    m_mach_port_server->on_receive_backing_stores = [](MachPortServer::BackingStoresMessage message) {
        if (auto view = WebContentClient::view_for_pid_and_page_id(message.pid, message.page_id); view.has_value())
            view->did_allocate_iosurface_backing_stores(message.front_backing_store_id, move(message.front_backing_store_port), message.back_backing_store_id, move(message.back_backing_store_port));
    };
#endif

    Vector<ByteString> raw_urls;
    Vector<ByteString> certificates;
    Optional<HeadlessMode> headless_mode;
    Optional<int> window_width;
    Optional<int> window_height;
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
    Optional<StringView> default_time_zone;
    Optional<u16> dns_server_port;
    bool use_dns_over_tls = false;
    bool enable_test_mode = false;
    bool validate_dnssec_locally = false;
    bool log_all_js_exceptions = false;
    bool disable_site_isolation = false;
    bool enable_idl_tracing = false;
    bool disable_http_memory_cache = false;
    bool disable_http_disk_cache = false;
    bool disable_content_filter = false;
    Optional<StringView> resource_substitution_map_path;
    bool enable_autoplay = false;
    bool expose_internals_object = false;
    bool force_cpu_painting = false;
    bool force_fontconfig = false;
    bool collect_garbage_on_every_allocation = false;
    bool disable_scrollbar_painting = false;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("The Ladybird web browser :^)");
    args_parser.add_positional_argument(raw_urls, "URLs to open", "url", Core::ArgsParser::Required::No);

    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Optional,
        .help_string = "Run Ladybird without a browser window. Mode may be 'screenshot' (default), 'layout-tree', 'text', or 'manual'.",
        .long_name = "headless",
        .value_name = "mode",
        .accept_value = [&](StringView value) {
            if (headless_mode.has_value())
                return false;

            if (value.is_empty() || value.equals_ignoring_ascii_case("screenshot"sv))
                headless_mode = HeadlessMode::Screenshot;
            else if (value.equals_ignoring_ascii_case("layout-tree"sv))
                headless_mode = HeadlessMode::LayoutTree;
            else if (value.equals_ignoring_ascii_case("text"sv))
                headless_mode = HeadlessMode::Text;
            else if (value.equals_ignoring_ascii_case("manual"sv))
                headless_mode = HeadlessMode::Manual;

            return headless_mode.has_value();
        },
    });

    args_parser.add_option(window_width, "Set viewport width in pixels (default: 800) (currently only supported for headless mode)", "window-width", 0, "pixels");
    args_parser.add_option(window_height, "Set viewport height in pixels (default: 600) (currently only supported for headless mode)", "window-height", 0, "pixels");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(new_window, "Force opening in a new window", "new-window", 'n');
    args_parser.add_option(force_new_process, "Force creation of a new browser process", "force-new-process");
    args_parser.add_option(allow_popups, "Disable popup blocking by default", "allow-popups");
    args_parser.add_option(disable_scripting, "Disable scripting by default", "disable-scripting");
    args_parser.add_option(disable_sql_database, "Disable SQL database", "disable-sql-database");
    args_parser.add_option(debug_process, "Wait for a debugger to attach to the given process name (WebContent, RequestServer, etc.)", "debug-process", 0, "process-name");
    args_parser.add_option(profile_process, "Enable callgrind profiling of the given process name (WebContent, RequestServer, etc.)", "profile-process", 0, "process-name");
    args_parser.add_option(webdriver_content_ipc_path, "Path to WebDriver IPC for WebContent", "webdriver-content-path", 0, "path", Core::ArgsParser::OptionHideMode::CommandLineAndMarkdown);
    args_parser.add_option(enable_test_mode, "Enable test mode", "test-mode");
    args_parser.add_option(log_all_js_exceptions, "Log all JavaScript exceptions", "log-all-js-exceptions");
    args_parser.add_option(disable_site_isolation, "Disable site isolation", "disable-site-isolation");
    args_parser.add_option(enable_idl_tracing, "Enable IDL tracing", "enable-idl-tracing");
    args_parser.add_option(disable_http_memory_cache, "Disable HTTP memory cache", "disable-http-memory-cache");
    args_parser.add_option(disable_http_disk_cache, "Disable HTTP disk cache", "disable-http-disk-cache");
    args_parser.add_option(disable_content_filter, "Disable content filter", "disable-content-filter");
    args_parser.add_option(enable_autoplay, "Enable multimedia autoplay", "enable-autoplay");
    args_parser.add_option(expose_internals_object, "Expose internals object", "expose-internals-object");
    args_parser.add_option(force_cpu_painting, "Force CPU painting", "force-cpu-painting");
    args_parser.add_option(force_fontconfig, "Force using fontconfig for font loading", "force-fontconfig");
    args_parser.add_option(collect_garbage_on_every_allocation, "Collect garbage after every JS heap allocation", "collect-garbage-on-every-allocation", 'g');
    args_parser.add_option(disable_scrollbar_painting, "Don't paint horizontal or vertical scrollbars on the main viewport", "disable-scrollbar-painting");
    args_parser.add_option(dns_server_address, "Set the DNS server address", "dns-server", 0, "host|address");
    args_parser.add_option(dns_server_port, "Set the DNS server port", "dns-port", 0, "port (default: 53 or 853 if --dot)");
    args_parser.add_option(use_dns_over_tls, "Use DNS over TLS", "dot");
    args_parser.add_option(validate_dnssec_locally, "Validate DNSSEC locally", "dnssec");
    args_parser.add_option(default_time_zone, "Default time zone", "default-time-zone", 0, "time-zone-id");
    args_parser.add_option(resource_substitution_map_path, "Path to JSON file mapping URLs to local files", "resource-map", 0, "path");

    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Optional,
        .help_string = "Enable the Firefox DevTools server, with an optional port",
        .long_name = "devtools",
        .value_name = "port",
        .accept_value = [&](StringView value) {
            if (value.is_empty())
                devtools_port = WebView::default_devtools_port;
            else
                devtools_port = value.to_number<u16>();

            return devtools_port.has_value();
        },
    });

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
    args_parser.parse(m_arguments);

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

    // Disable site isolation when debugging WebContent. Otherwise, the process swap may interfere with the gdb session.
    if (debug_process_type == ProcessType::WebContent)
        disable_site_isolation = true;

    m_browser_options = {
        .urls = sanitize_urls(raw_urls, m_settings.new_tab_page_url()),
        .raw_urls = move(raw_urls),
        .headless_mode = headless_mode,
        .new_window = new_window ? NewWindow::Yes : NewWindow::No,
        .force_new_process = force_new_process ? ForceNewProcess::Yes : ForceNewProcess::No,
        .allow_popups = allow_popups ? AllowPopups::Yes : AllowPopups::No,
        .disable_scripting = disable_scripting ? DisableScripting::Yes : DisableScripting::No,
        .disable_sql_database = disable_sql_database ? DisableSQLDatabase::Yes : DisableSQLDatabase::No,
        .debug_helper_process = move(debug_process_type),
        .profile_helper_process = move(profile_process_type),
        .dns_settings = (dns_server_address.has_value()
                ? Optional<DNSSettings> { use_dns_over_tls
                          ? DNSSettings(DNSOverTLS(dns_server_address.release_value(), *dns_server_port, validate_dnssec_locally))
                          : DNSSettings(DNSOverUDP(dns_server_address.release_value(), *dns_server_port, validate_dnssec_locally)) }
                : OptionalNone()),
        .devtools_port = devtools_port,
        .enable_content_filter = disable_content_filter ? EnableContentFilter::No : EnableContentFilter::Yes,
    };

    if (window_width.has_value())
        m_browser_options.window_width = *window_width;
    if (window_height.has_value())
        m_browser_options.window_height = *window_height;

    if (webdriver_content_ipc_path.has_value())
        m_browser_options.webdriver_content_ipc_path = *webdriver_content_ipc_path;

    auto http_disk_cache_mode = HTTPDiskCacheMode::Enabled;
    if (disable_http_disk_cache)
        http_disk_cache_mode = HTTPDiskCacheMode::Disabled;
    else if (force_new_process)
        http_disk_cache_mode = HTTPDiskCacheMode::Partitioned;

    m_request_server_options = {
        .certificates = move(certificates),
        .http_disk_cache_mode = http_disk_cache_mode,
        .resource_substitution_map_path = resource_substitution_map_path.has_value() ? Optional<ByteString> { *resource_substitution_map_path } : OptionalNone {},
    };

    m_web_content_options = {
        .command_line = MUST(String::join(' ', m_arguments.strings)),
        .executable_path = MUST(String::from_byte_string(MUST(Core::System::current_executable_path()))),
        .user_agent_preset = move(user_agent_preset),
        .is_test_mode = enable_test_mode ? IsTestMode::Yes : IsTestMode::No,
        .log_all_js_exceptions = log_all_js_exceptions ? LogAllJSExceptions::Yes : LogAllJSExceptions::No,
        .disable_site_isolation = disable_site_isolation ? DisableSiteIsolation::Yes : DisableSiteIsolation::No,
        .enable_idl_tracing = enable_idl_tracing ? EnableIDLTracing::Yes : EnableIDLTracing::No,
        .enable_http_memory_cache = disable_http_memory_cache ? EnableMemoryHTTPCache::No : EnableMemoryHTTPCache::Yes,
        .expose_internals_object = expose_internals_object ? ExposeInternalsObject::Yes : ExposeInternalsObject::No,
        .force_cpu_painting = force_cpu_painting ? ForceCPUPainting::Yes : ForceCPUPainting::No,
        .force_fontconfig = force_fontconfig ? ForceFontconfig::Yes : ForceFontconfig::No,
        .enable_autoplay = enable_autoplay ? EnableAutoplay::Yes : EnableAutoplay::No,
        .collect_garbage_on_every_allocation = collect_garbage_on_every_allocation ? CollectGarbageOnEveryAllocation::Yes : CollectGarbageOnEveryAllocation::No,
        .paint_viewport_scrollbars = disable_scrollbar_painting ? PaintViewportScrollbars::No : PaintViewportScrollbars::Yes,
        .default_time_zone = default_time_zone,
    };

    create_platform_options(m_browser_options, m_request_server_options, m_web_content_options);
    initialize_actions();

    m_event_loop = create_platform_event_loop();
    TRY(launch_services());

    return {};
}

void Application::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab) const
{
    if (auto view = open_blank_new_tab(activate_tab); view.has_value())
        view->load(url);
}

static ErrorOr<NonnullRefPtr<WebContentClient>> create_web_content_client(Optional<ViewImplementation&> view)
{
    auto request_server_socket = TRY(connect_new_request_server_client());
    auto image_decoder_socket = TRY(connect_new_image_decoder_client());

    if (view.has_value())
        return WebView::launch_web_content_process(*view, move(image_decoder_socket), move(request_server_socket));
    return WebView::launch_spare_web_content_process(move(image_decoder_socket), move(request_server_socket));
}

ErrorOr<NonnullRefPtr<WebContentClient>> Application::launch_web_content_process(ViewImplementation& view)
{
    if (m_spare_web_content_process) {
        auto web_content_client = m_spare_web_content_process.release_nonnull();
        launch_spare_web_content_process();

        web_content_client->assign_view({}, view);
        return web_content_client;
    }

    launch_spare_web_content_process();
    return create_web_content_client(view);
}

void Application::launch_spare_web_content_process()
{
    // Disable spare processes when debugging WebContent. Otherwise, it breaks running `gdb attach -p $(pidof WebContent)`.
    if (browser_options().debug_helper_process == ProcessType::WebContent)
        return;
    // Disable spare processes when profiling WebContent. This reduces callgrind logging we are not interested in.
    if (browser_options().profile_helper_process == ProcessType::WebContent)
        return;

    if (m_has_queued_task_to_launch_spare_web_content_process)
        return;
    m_has_queued_task_to_launch_spare_web_content_process = true;

    Core::deferred_invoke([this]() {
        m_has_queued_task_to_launch_spare_web_content_process = false;

        auto web_content_client = create_web_content_client({});
        if (web_content_client.is_error()) {
            dbgln("Unable to create spare web content client: {}", web_content_client.error());
            return;
        }

        m_spare_web_content_process = web_content_client.release_value();

        if (auto process = find_process(m_spare_web_content_process->pid()); process.has_value())
            process->set_title("(spare)"_utf16);
    });
}

ErrorOr<void> Application::launch_services()
{
    m_settings_observer = make<ApplicationSettingsObserver>();

    m_process_manager = make<ProcessManager>();
    m_process_manager->on_process_exited = [this](Process&& process) {
        process_did_exit(move(process));
    };

    if (m_browser_options.disable_sql_database == DisableSQLDatabase::No) {
        // FIXME: Move this to a generic "Ladybird data directory" helper.
        auto database_path = ByteString::formatted("{}/Ladybird", Core::StandardPaths::user_data_directory());

        m_database = TRY(Database::Database::create(database_path, "Ladybird"sv));
        m_cookie_jar = TRY(CookieJar::create(*m_database));
        m_storage_jar = TRY(StorageJar::create(*m_database));
    } else {
        m_cookie_jar = CookieJar::create();
        m_storage_jar = StorageJar::create();
    }

    // No need to monitor the system time zone if the TZ environment variable is set, as it overrides system preferences.
    if (!Core::Environment::has("TZ"sv)) {
        if (auto time_zone_watcher = Core::TimeZoneWatcher::create(); time_zone_watcher.is_error()) {
            warnln("Unable to monitor system time zone: {}", time_zone_watcher.error());
        } else {
            m_time_zone_watcher = time_zone_watcher.release_value();

            m_time_zone_watcher->on_time_zone_changed = [] {
                WebContentClient::for_each_client([&](WebView::WebContentClient& client) {
                    client.async_system_time_zone_changed();
                    return IterationDecision::Continue;
                });
            };
        }
    }

    TRY(launch_request_server());
    TRY(launch_image_decoder_server());

    if (m_browser_options.devtools_port.has_value())
        TRY(launch_devtools_server());

    return {};
}

ErrorOr<void> Application::launch_request_server()
{
    m_request_server_client = TRY(launch_request_server_process());

    m_request_server_client->on_retrieve_http_cookie = [this](URL::URL const& url) {
        return m_cookie_jar->get_cookie(url, HTTP::Cookie::Source::Http);
    };

    m_request_server_client->on_request_server_died = [this]() {
        m_request_server_client = nullptr;

        if (Core::EventLoop::current().was_exit_requested())
            return;

        if (auto result = launch_request_server(); result.is_error()) {
            warnln("\033[31;1mUnable to launch replacement RequestServer: {}\033[0m", result.error());
            VERIFY_NOT_REACHED();
        }

        auto client_count = WebContentClient::client_count();
        auto request_server_sockets = m_request_server_client->send_sync_but_allow_failure<Messages::RequestServer::ConnectNewClients>(client_count);
        if (!request_server_sockets || request_server_sockets->sockets().is_empty()) {
            warnln("\033Failed to connect {} new clients to ImageDecoder\033[0m", client_count);
            VERIFY_NOT_REACHED();
        }

        WebContentClient::for_each_client([sockets = request_server_sockets->take_sockets()](WebContentClient& client) mutable {
            client.async_connect_to_request_server(sockets.take_last());
            return IterationDecision::Continue;
        });
    };

    if (m_browser_options.dns_settings.has_value())
        m_settings.set_dns_settings(m_browser_options.dns_settings.value(), true);

    return {};
}

ErrorOr<void> Application::launch_image_decoder_server()
{
    m_image_decoder_client = TRY(launch_image_decoder_process());

    m_image_decoder_client->on_death = [this]() {
        m_image_decoder_client = nullptr;

        if (Core::EventLoop::current().was_exit_requested())
            return;

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
    VERIFY(!m_devtools);

    if (!m_browser_options.devtools_port.has_value())
        m_browser_options.devtools_port = WebView::default_devtools_port;

    m_devtools = TRY(DevTools::DevToolsServer::create(*this, *m_browser_options.devtools_port));
    on_devtools_enabled();

    return {};
}

static NonnullRefPtr<Core::Timer> load_page_for_screenshot_and_exit(Core::EventLoop& event_loop, HeadlessWebView& view, URL::URL const& url, int screenshot_timeout)
{
    outln("Taking screenshot after {} seconds", screenshot_timeout);

    auto timer = Core::Timer::create_single_shot(
        screenshot_timeout * 1000,
        [&]() {
            view.take_screenshot(ViewImplementation::ScreenshotType::Full)
                ->when_resolved([&event_loop](auto const& path) {
                    outln("Saved screenshot to: {}", path);
                    event_loop.quit(0);
                })
                .when_rejected([&event_loop](auto const& error) {
                    warnln("Unable to take screenshot: {}", error);
                    event_loop.quit(0);
                });
        });

    view.load(url);
    timer->start();

    return timer;
}

static void load_page_for_info_and_exit(Core::EventLoop& event_loop, HeadlessWebView& view, URL::URL const& url, WebView::PageInfoType type)
{
    view.on_load_finish = [&view, &event_loop, url, type](auto const& loaded_url) {
        if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
            return;

        view.request_internal_page_info(type)->when_resolved([&event_loop](auto const& text) {
            outln("{}", text);
            event_loop.quit(0);
        });
    };

    view.load(url);
}

static void load_page_and_exit_on_close(Core::EventLoop& event_loop, HeadlessWebView& view, URL::URL const& url)
{
    view.on_close = [&event_loop]() {
        event_loop.quit(0);
    };

    view.load(url);
}

ErrorOr<int> Application::execute()
{
    OwnPtr<HeadlessWebView> view;
    RefPtr<Core::Timer> screenshot_timer;

    if (m_browser_options.headless_mode.has_value()) {
        auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
        auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

        view = HeadlessWebView::create(move(theme), { m_browser_options.window_width, m_browser_options.window_height });

        if (!m_browser_options.webdriver_content_ipc_path.has_value()) {
            if (m_browser_options.urls.size() != 1)
                return Error::from_string_literal("Headless mode currently only supports exactly one URL");

            switch (*m_browser_options.headless_mode) {
            case HeadlessMode::Screenshot:
                screenshot_timer = load_page_for_screenshot_and_exit(*m_event_loop, *view, m_browser_options.urls.first(), 1);
                break;
            case HeadlessMode::LayoutTree:
                load_page_for_info_and_exit(*m_event_loop, *view, m_browser_options.urls.first(), WebView::PageInfoType::LayoutTree | WebView::PageInfoType::PaintTree);
                break;
            case HeadlessMode::Text:
                load_page_for_info_and_exit(*m_event_loop, *view, m_browser_options.urls.first(), WebView::PageInfoType::Text);
                break;
            case HeadlessMode::Manual:
                load_page_and_exit_on_close(*m_event_loop, *view, m_browser_options.urls.first());
                break;
            case HeadlessMode::Test:
                VERIFY_NOT_REACHED();
            }
        }
    }

    return m_event_loop->exec();
}

NonnullOwnPtr<Core::EventLoop> Application::create_platform_event_loop()
{
    return make<Core::EventLoop>();
}

void Application::add_child_process(WebView::Process&& process)
{
    m_process_manager->add_process(move(process));
}

#if defined(AK_OS_MACH)
void Application::set_process_mach_port(pid_t pid, Core::MachPort&& port)
{
    m_process_manager->set_process_mach_port(pid, move(port));
}
#endif

Optional<Process&> Application::find_process(pid_t pid)
{
    return m_process_manager->find_process(pid);
}

void Application::process_did_exit(Process&& process)
{
    if (m_event_loop->was_exit_requested())
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
        if (auto client = process.client<Requests::RequestClient>(); client.has_value()) {
            dbgln_if(WEBVIEW_PROCESS_DEBUG, "Restart request server");
            if (auto on_request_server_died = move(client->on_request_server_died))
                on_request_server_died();
        }
        break;
    case ProcessType::WebContent:
        if (auto client = process.client<WebContentClient>(); client.has_value())
            client->notify_all_views_of_crash();
        break;
    case ProcessType::WebWorker:
        dbgln_if(WEBVIEW_PROCESS_DEBUG, "WebWorker {} died, not sure what to do.", process.pid());
        break;
    case ProcessType::Browser:
        dbgln("Invalid process type to be dying: Browser");
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<LexicalPath> Application::path_for_downloaded_file(StringView file) const
{
    if (browser_options().headless_mode.has_value()) {
        auto downloads_directory = Core::StandardPaths::downloads_directory();

        if (!FileSystem::is_directory(downloads_directory)) {
            dbgln("Unable to ask user for download folder in headless mode, please ensure {} is a directory or use the XDG_DOWNLOAD_DIR environment variable to set a new download directory", downloads_directory);
            return Error::from_errno(ENOENT);
        }

        return LexicalPath::join(downloads_directory, file);
    }

    auto download_path = ask_user_for_download_path(file);
    if (!download_path.has_value())
        return Error::from_errno(ECANCELED);

    return LexicalPath { download_path.release_value() };
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    outln("{} saved to: {}", download_name, path);
}

void Application::display_error_dialog(StringView error_message) const
{
    warnln("{}", error_message);
}

Utf16String Application::clipboard_text() const
{
    if (!m_clipboard.has_value())
        return {};
    if (m_clipboard->mime_type != "text/plain"sv)
        return {};
    return Utf16String::from_utf8(m_clipboard->data);
}

Vector<Web::Clipboard::SystemClipboardRepresentation> Application::clipboard_entries() const
{
    if (!m_clipboard.has_value())
        return {};
    return { *m_clipboard };
}

void Application::insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation entry)
{
    m_clipboard = move(entry);
}

NonnullRefPtr<Core::Promise<Application::BrowsingDataSizes>> Application::estimate_browsing_data_size_accessed_since(UnixDateTime since)
{
    auto promise = Core::Promise<BrowsingDataSizes>::construct();

    m_request_server_client->estimate_cache_size_accessed_since(since)
        ->when_resolved([this, promise, since](Requests::CacheSizes cache_sizes) {
            auto cookie_sizes = m_cookie_jar->estimate_storage_size_accessed_since(since);
            auto storage_sizes = m_storage_jar->estimate_storage_size_accessed_since(since);

            BrowsingDataSizes sizes;

            sizes.cache_size_since_requested_time = cache_sizes.since_requested_time;
            sizes.total_cache_size = cache_sizes.total;

            sizes.site_data_size_since_requested_time = cookie_sizes.since_requested_time + storage_sizes.since_requested_time;
            sizes.total_site_data_size = cookie_sizes.total + storage_sizes.total;

            promise->resolve(sizes);
        })
        .when_rejected([promise](Error& error) {
            promise->reject(move(error));
        });

    return promise;
}

void Application::clear_browsing_data(ClearBrowsingDataOptions const& options)
{
    if (options.delete_cached_files == ClearBrowsingDataOptions::Delete::Yes) {
        m_request_server_client->async_remove_cache_entries_accessed_since(options.since);

        // FIXME: Maybe we should forward the "since" parameter to the WebContent process, but the in-memory cache is
        //        transient anyways, so just assuming they were all accessed in the last hour is fine for now.
        ViewImplementation::for_each_view([](ViewImplementation& view) {
            // FIXME: This should be promoted from a debug request to a proper endpoint.
            view.debug_request("clear-cache"sv);
            return IterationDecision::Continue;
        });
    }

    if (options.delete_site_data == ClearBrowsingDataOptions::Delete::Yes) {
        m_cookie_jar->expire_cookies_accessed_since(options.since);
        m_storage_jar->remove_items_accessed_since(options.since);
    }
}

void Application::initialize_actions()
{
    auto debug_request = [this](auto request) {
        return [this, request]() {
            if (auto view = active_web_view(); view.has_value())
                view->debug_request(request);
        };
    };

    auto check = [](auto& action, auto request) {
        return [&action, request]() {
            ViewImplementation::for_each_view([checked = action->checked(), request](ViewImplementation& view) {
                view.debug_request(request, checked ? "on"sv : "off"sv);
                return IterationDecision::Continue;
            });
        };
    };

    auto add_spoofed_value = [](auto& menu, auto name, auto value, auto& cached_value, auto request) {
        auto action = Action::create_checkable(name, ActionID::SpoofUserAgent, [value, &cached_value, request]() {
            cached_value = value;

            ViewImplementation::for_each_view([&](ViewImplementation& view) {
                view.debug_request(request, cached_value);
                view.debug_request("clear-cache"sv); // Clear the cache to ensure requests are re-done with the new value.
                return IterationDecision::Continue;
            });
        });

        action->set_checked(value == cached_value);
        menu->add_action(move(action));
    };

    m_reload_action = Action::create("Reload"sv, ActionID::Reload, [this]() {
        if (auto view = active_web_view(); view.has_value())
            view->reload();
    });

    m_copy_selection_action = Action::create("Copy"sv, ActionID::CopySelection, [this]() {
        if (auto view = active_web_view(); view.has_value())
            if (!view->selected_text().is_empty())
                insert_clipboard_entry({ view->selected_text(), "text/plain"_string });
    });
    m_paste_action = Action::create("Paste"sv, ActionID::Paste, [this]() {
        if (auto view = active_web_view(); view.has_value())
            view->paste_text_from_clipboard();
    });
    m_select_all_action = Action::create("Select All"sv, ActionID::SelectAll, [this]() {
        if (auto view = active_web_view(); view.has_value())
            view->select_all();
    });

    m_open_about_page_action = Action::create("About Ladybird"sv, ActionID::OpenAboutPage, [this]() {
        open_url_in_new_tab(URL::about_version(), Web::HTML::ActivateTab::Yes);
    });
    m_open_settings_page_action = Action::create("Settings"sv, ActionID::OpenSettingsPage, [this]() {
        open_url_in_new_tab(URL::about_settings(), Web::HTML::ActivateTab::Yes);
    });

    m_zoom_menu = Menu::create_group("Zoom"sv);
    m_zoom_menu->add_action(Action::create("Zoom In"sv, ActionID::ZoomIn, [this]() {
        if (auto view = active_web_view(); view.has_value())
            view->zoom_in();
    }));
    m_zoom_menu->add_action(Action::create("Zoom Out"sv, ActionID::ZoomOut, [this]() {
        if (auto view = active_web_view(); view.has_value())
            view->zoom_out();
    }));

    m_reset_zoom_action = Action::create("Reset Zoom"sv, ActionID::ResetZoom, [this]() {
        if (auto view = active_web_view(); view.has_value())
            view->reset_zoom();
    });
    m_zoom_menu->add_action(*m_reset_zoom_action);

    auto set_color_scheme = [this](auto color_scheme) {
        return [this, color_scheme]() {
            m_color_scheme = color_scheme;

            ViewImplementation::for_each_view([&](ViewImplementation& view) {
                view.set_preferred_color_scheme(m_color_scheme);
                return IterationDecision::Continue;
            });
        };
    };

    m_color_scheme_menu = Menu::create_group("Color Scheme"sv);
    m_color_scheme_menu->add_action(Action::create_checkable("Auto"sv, ActionID::PreferredColorScheme, set_color_scheme(Web::CSS::PreferredColorScheme::Auto)));
    m_color_scheme_menu->add_action(Action::create_checkable("Dark"sv, ActionID::PreferredColorScheme, set_color_scheme(Web::CSS::PreferredColorScheme::Dark)));
    m_color_scheme_menu->add_action(Action::create_checkable("Light"sv, ActionID::PreferredColorScheme, set_color_scheme(Web::CSS::PreferredColorScheme::Light)));
    m_color_scheme_menu->items().first().get<NonnullRefPtr<Action>>()->set_checked(true);

    auto set_contrast = [this](auto contrast) {
        return [this, contrast]() {
            m_contrast = contrast;

            ViewImplementation::for_each_view([&](ViewImplementation& view) {
                view.set_preferred_contrast(m_contrast);
                return IterationDecision::Continue;
            });
        };
    };

    m_contrast_menu = Menu::create_group("Contrast"sv);
    m_contrast_menu->add_action(Action::create_checkable("Auto"sv, ActionID::PreferredContrast, set_contrast(Web::CSS::PreferredContrast::Auto)));
    m_contrast_menu->add_action(Action::create_checkable("Less"sv, ActionID::PreferredContrast, set_contrast(Web::CSS::PreferredContrast::Less)));
    m_contrast_menu->add_action(Action::create_checkable("More"sv, ActionID::PreferredContrast, set_contrast(Web::CSS::PreferredContrast::More)));
    m_contrast_menu->add_action(Action::create_checkable("No Preference"sv, ActionID::PreferredContrast, set_contrast(Web::CSS::PreferredContrast::NoPreference)));
    m_contrast_menu->items().first().get<NonnullRefPtr<Action>>()->set_checked(true);

    auto set_motion = [this](auto motion) {
        return [this, motion]() {
            m_motion = motion;

            ViewImplementation::for_each_view([&](ViewImplementation& view) {
                view.set_preferred_motion(m_motion);
                return IterationDecision::Continue;
            });
        };
    };

    m_motion_menu = Menu::create_group("Motion"sv);
    m_motion_menu->add_action(Action::create_checkable("Auto"sv, ActionID::PreferredMotion, set_motion(Web::CSS::PreferredMotion::Auto)));
    m_motion_menu->add_action(Action::create_checkable("Reduce"sv, ActionID::PreferredMotion, set_motion(Web::CSS::PreferredMotion::Reduce)));
    m_motion_menu->add_action(Action::create_checkable("No Preference"sv, ActionID::PreferredMotion, set_motion(Web::CSS::PreferredMotion::NoPreference)));
    m_motion_menu->items().first().get<NonnullRefPtr<Action>>()->set_checked(true);

    m_inspect_menu = Menu::create("Inspect"sv);

    m_view_source_action = Action::create("View Source"sv, ActionID::ViewSource, [this]() {
        if (auto view = active_web_view(); view.has_value())
            view->get_source();
    });
    m_inspect_menu->add_action(*m_view_source_action);

    m_inspect_menu->add_action(Action::create("Open Task Manager"sv, ActionID::OpenProcessesPage, [this]() {
        open_url_in_new_tab(URL::about_processes(), Web::HTML::ActivateTab::Yes);
    }));

    m_toggle_devtools_action = Action::create("Enable DevTools"sv, ActionID::ToggleDevTools, [this]() {
        if (auto result = toggle_devtools_enabled(); result.is_error())
            display_error_dialog(MUST(String::formatted("Unable to start DevTools: {}", result.error())));
    });
    m_inspect_menu->add_action(*m_toggle_devtools_action);

    m_debug_menu = Menu::create("Debug"sv);
    m_debug_menu->add_action(Action::create("Dump Session History Tree"sv, ActionID::DumpSessionHistoryTree, debug_request("dump-session-history"sv)));
    m_debug_menu->add_action(Action::create("Dump DOM Tree"sv, ActionID::DumpDOMTree, debug_request("dump-dom-tree"sv)));
    m_debug_menu->add_action(Action::create("Dump Layout Tree"sv, ActionID::DumpLayoutTree, debug_request("dump-layout-tree"sv)));
    m_debug_menu->add_action(Action::create("Dump Paint Tree"sv, ActionID::DumpPaintTree, debug_request("dump-paint-tree"sv)));
    m_debug_menu->add_action(Action::create("Dump Stacking Context Tree"sv, ActionID::DumpStackingContextTree, debug_request("dump-stacking-context-tree"sv)));
    m_debug_menu->add_action(Action::create("Dump Display List"sv, ActionID::DumpDisplayList, debug_request("dump-display-list"sv)));
    m_debug_menu->add_action(Action::create("Dump Style Sheets"sv, ActionID::DumpStyleSheets, debug_request("dump-style-sheets"sv)));
    m_debug_menu->add_action(Action::create("Dump All Resolved Styles"sv, ActionID::DumpStyles, debug_request("dump-all-resolved-styles"sv)));
    m_debug_menu->add_action(Action::create("Dump CSS Errors"sv, ActionID::DumpCSSErrors, debug_request("dump-all-css-errors"sv)));
    m_debug_menu->add_action(Action::create("Dump Cookies"sv, ActionID::DumpCookies, [this]() { m_cookie_jar->dump_cookies(); }));
    m_debug_menu->add_action(Action::create("Dump Local Storage"sv, ActionID::DumpLocalStorage, debug_request("dump-local-storage"sv)));
    m_debug_menu->add_action(Action::create("Dump GC graph"sv, ActionID::DumpGCGraph, [this]() {
        if (auto view = active_web_view(); view.has_value()) {
            auto gc_graph_path = view->dump_gc_graph();
            if (gc_graph_path.is_error()) {
                warnln("\033[31;1mFailed to dump GC graph: {}\033[0m", gc_graph_path.error());
            } else {
                warnln("\033[33;1mDumped GC graph into {}\033[0m", gc_graph_path.value());
                if (auto source_dir = Core::Environment::get("LADYBIRD_SOURCE_DIR"sv); source_dir.has_value())
                    warnln("\033[33;1mGC graph explorer: file://{}/Meta/gc-heap-explorer.html?script=file://{}\033[0m", *source_dir, gc_graph_path.value());
            }
        }
    }));
    m_debug_menu->add_separator();

    m_show_line_box_borders_action = Action::create_checkable("Show Line Box Borders"sv, ActionID::ShowLineBoxBorders, check(m_show_line_box_borders_action, "set-line-box-borders"sv));
    m_debug_menu->add_action(*m_show_line_box_borders_action);
    m_debug_menu->add_separator();

    m_debug_menu->add_action(Action::create("Collect Garbage"sv, ActionID::CollectGarbage, debug_request("collect-garbage"sv)));
    m_debug_menu->add_separator();

    auto spoof_user_agent_menu = Menu::create_group("Spoof User Agent"sv);
    m_user_agent_string = m_web_content_options.user_agent_preset.has_value()
        ? *WebView::user_agents.get(*m_web_content_options.user_agent_preset)
        : Web::default_user_agent;

    add_spoofed_value(spoof_user_agent_menu, "Disabled"sv, Web::default_user_agent, m_user_agent_string, "spoof-user-agent"sv);
    for (auto const& user_agent : WebView::user_agents)
        add_spoofed_value(spoof_user_agent_menu, user_agent.key, user_agent.value, m_user_agent_string, "spoof-user-agent"sv);

    auto navigator_compatibility_mode_menu = Menu::create_group("Navigator Compatibility Mode"sv);
    m_navigator_compatibility_mode = "chrome"sv;

    add_spoofed_value(navigator_compatibility_mode_menu, "Chrome"sv, "chrome"sv, m_navigator_compatibility_mode, "navigator-compatibility-mode"sv);
    add_spoofed_value(navigator_compatibility_mode_menu, "Gecko"sv, "gecko"sv, m_navigator_compatibility_mode, "navigator-compatibility-mode"sv);
    add_spoofed_value(navigator_compatibility_mode_menu, "WebKit"sv, "webkit"sv, m_navigator_compatibility_mode, "navigator-compatibility-mode"sv);

    m_debug_menu->add_submenu(move(spoof_user_agent_menu));
    m_debug_menu->add_submenu(move(navigator_compatibility_mode_menu));
    m_debug_menu->add_separator();

    m_enable_scripting_action = Action::create_checkable("Enable Scripting"sv, ActionID::EnableScripting, check(m_enable_scripting_action, "scripting"sv));
    m_enable_scripting_action->set_checked(m_browser_options.disable_scripting == WebView::DisableScripting::No);
    m_debug_menu->add_action(*m_enable_scripting_action);

    m_enable_content_filtering_action = Action::create_checkable("Enable Content Filtering"sv, ActionID::EnableContentFiltering, check(m_enable_content_filtering_action, "content-filtering"sv));
    m_enable_content_filtering_action->set_checked(m_browser_options.enable_content_filter == WebView::EnableContentFilter::Yes);
    m_debug_menu->add_action(*m_enable_content_filtering_action);

    m_block_pop_ups_action = Action::create_checkable("Block Pop-ups"sv, ActionID::BlockPopUps, check(m_block_pop_ups_action, "block-pop-ups"sv));
    m_block_pop_ups_action->set_checked(m_browser_options.allow_popups == AllowPopups::No);
    m_debug_menu->add_action(*m_block_pop_ups_action);
}

void Application::apply_view_options(Badge<ViewImplementation>, ViewImplementation& view)
{
    view.set_preferred_color_scheme(m_color_scheme);
    view.set_preferred_contrast(m_contrast);
    view.set_preferred_motion(m_motion);

    view.debug_request("set-line-box-borders"sv, m_show_line_box_borders_action->checked() ? "on"sv : "off"sv);
    view.debug_request("scripting"sv, m_enable_scripting_action->checked() ? "on"sv : "off"sv);
    view.debug_request("content-filtering"sv, m_enable_content_filtering_action->checked() ? "on"sv : "off"sv);
    view.debug_request("block-pop-ups"sv, m_block_pop_ups_action->checked() ? "on"sv : "off"sv);
    view.debug_request("spoof-user-agent"sv, m_user_agent_string);
    view.debug_request("navigator-compatibility-mode"sv, m_navigator_compatibility_mode);
}

ErrorOr<void> Application::toggle_devtools_enabled()
{
    if (m_devtools) {
        m_devtools.clear();
        on_devtools_disabled();
    } else {
        TRY(launch_devtools_server());
    }

    return {};
}

void Application::on_devtools_enabled() const
{
    m_toggle_devtools_action->set_text("Disable DevTools"sv);
}

void Application::on_devtools_disabled() const
{
    m_toggle_devtools_action->set_text("Enable DevTools"sv);
}

void Application::refresh_tab_list()
{
    if (!m_devtools)
        return;
    m_devtools->refresh_tab_list();
}

Optional<Core::TimeZoneWatcher&> Application::time_zone_watcher()
{
    if (m_time_zone_watcher != nullptr)
        return *m_time_zone_watcher;
    return {};
}

Vector<DevTools::TabDescription> Application::tab_list() const
{
    Vector<DevTools::TabDescription> tabs;

    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        tabs.empend(view.view_id(), view.title().to_utf8(), view.url().to_string());
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

void Application::inspect_accessibility_tree(DevTools::TabDescription const& description, OnAccessibilityTreeInspectionComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->on_received_accessibility_tree = [&view = *view, on_complete = move(on_complete)](JsonObject accessibility_tree) {
        view.on_received_accessibility_tree = nullptr;
        on_complete(move(accessibility_tree));
    };

    view->inspect_accessibility_tree();
}

void Application::listen_for_dom_properties(DevTools::TabDescription const& description, OnDOMNodePropertiesReceived on_dom_node_properties_received) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_received_dom_node_properties = move(on_dom_node_properties_received);
}

void Application::stop_listening_for_dom_properties(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_received_dom_node_properties = nullptr;
}

void Application::inspect_dom_node(DevTools::TabDescription const& description, DOMNodeProperties::Type property_type, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->inspect_dom_node(node_id, property_type, pseudo_element);
}

void Application::clear_inspected_dom_node(DevTools::TabDescription const& description) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->clear_inspected_dom_node();
}

void Application::highlight_dom_node(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->highlight_dom_node(node_id, pseudo_element);
}

void Application::clear_highlighted_dom_node(DevTools::TabDescription const& description) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->clear_highlighted_dom_node();
}

void Application::listen_for_dom_mutations(DevTools::TabDescription const& description, OnDOMMutationReceived on_dom_mutation_received) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_dom_mutation_received = move(on_dom_mutation_received);
    view->set_listen_for_dom_mutations(true);
}

void Application::stop_listening_for_dom_mutations(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_dom_mutation_received = nullptr;
    view->set_listen_for_dom_mutations(false);
}

template<typename Edit>
static void edit_dom_node(DevTools::TabDescription const& description, Application::OnDOMNodeEditComplete on_complete, Edit&& edit)
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->on_finished_editing_dom_node = [&view = *view, on_complete = move(on_complete)](auto node_id) {
        view.on_finished_editing_dom_node = nullptr;

        if (node_id.has_value())
            on_complete(*node_id);
        else
            on_complete(Error::from_string_literal("Unable to find DOM node to edit"));
    };

    edit(*view);
}

void Application::get_dom_node_inner_html(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, OnDOMNodeHTMLReceived on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->on_received_dom_node_html = [&view = *view, on_complete = move(on_complete)](auto html) {
        view.on_received_dom_node_html = nullptr;
        on_complete(html);
    };

    view->get_dom_node_inner_html(node_id);
}

void Application::get_dom_node_outer_html(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, OnDOMNodeHTMLReceived on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->on_received_dom_node_html = [&view = *view, on_complete = move(on_complete)](auto html) {
        view.on_received_dom_node_html = nullptr;
        on_complete(html);
    };

    view->get_dom_node_outer_html(node_id);
}

void Application::set_dom_node_outer_html(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, String const& value, OnDOMNodeEditComplete on_complete) const
{
    edit_dom_node(description, move(on_complete), [&](auto& view) {
        view.set_dom_node_outer_html(node_id, value);
    });
}

void Application::set_dom_node_text(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, String const& value, OnDOMNodeEditComplete on_complete) const
{
    edit_dom_node(description, move(on_complete), [&](auto& view) {
        view.set_dom_node_text(node_id, value);
    });
}

void Application::set_dom_node_tag(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, String const& value, OnDOMNodeEditComplete on_complete) const
{
    edit_dom_node(description, move(on_complete), [&](auto& view) {
        view.set_dom_node_tag(node_id, value);
    });
}

void Application::add_dom_node_attributes(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, ReadonlySpan<Attribute> replacement_attributes, OnDOMNodeEditComplete on_complete) const
{
    edit_dom_node(description, move(on_complete), [&](auto& view) {
        view.add_dom_node_attributes(node_id, replacement_attributes);
    });
}

void Application::replace_dom_node_attribute(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, String const& name, ReadonlySpan<Attribute> replacement_attributes, OnDOMNodeEditComplete on_complete) const
{
    edit_dom_node(description, move(on_complete), [&](auto& view) {
        view.replace_dom_node_attribute(node_id, name, replacement_attributes);
    });
}

void Application::create_child_element(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, OnDOMNodeEditComplete on_complete) const
{
    edit_dom_node(description, move(on_complete), [&](auto& view) {
        view.create_child_element(node_id);
    });
}

void Application::insert_dom_node_before(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, Web::UniqueNodeID parent_node_id, Optional<Web::UniqueNodeID> sibling_node_id, OnDOMNodeEditComplete on_complete) const
{
    edit_dom_node(description, move(on_complete), [&](auto& view) {
        view.insert_dom_node_before(node_id, parent_node_id, sibling_node_id);
    });
}

void Application::clone_dom_node(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, OnDOMNodeEditComplete on_complete) const
{
    edit_dom_node(description, move(on_complete), [&](auto& view) {
        view.clone_dom_node(node_id);
    });
}

void Application::remove_dom_node(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, OnDOMNodeEditComplete on_complete) const
{
    edit_dom_node(description, move(on_complete), [&](auto& view) {
        view.remove_dom_node(node_id);
    });
}

void Application::retrieve_style_sheets(DevTools::TabDescription const& description, OnStyleSheetsReceived on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->on_received_style_sheet_list = [&view = *view, on_complete = move(on_complete)](Vector<Web::CSS::StyleSheetIdentifier> style_sheets) {
        view.on_received_style_sheet_list = nullptr;
        on_complete(move(style_sheets));
    };

    view->list_style_sheets();
}

void Application::retrieve_style_sheet_source(DevTools::TabDescription const& description, Web::CSS::StyleSheetIdentifier const& style_sheet) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->request_style_sheet_source(style_sheet);
}

void Application::listen_for_style_sheet_sources(DevTools::TabDescription const& description, OnStyleSheetSourceReceived on_style_sheet_source_received) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_received_style_sheet_source = [&view = *view, on_style_sheet_source_received = move(on_style_sheet_source_received)](auto const& style_sheet, auto const&, auto const& source) {
        on_style_sheet_source_received(style_sheet, source);
    };
}

void Application::stop_listening_for_style_sheet_sources(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_received_style_sheet_source = nullptr;
}

void Application::evaluate_javascript(DevTools::TabDescription const& description, String const& script, OnScriptEvaluationComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->on_received_js_console_result = [&view = *view, on_complete = move(on_complete)](JsonValue result) {
        view.on_received_js_console_result = nullptr;
        on_complete(move(result));
    };

    view->js_console_input(script);
}

void Application::listen_for_console_messages(DevTools::TabDescription const& description, OnConsoleMessage on_console_message) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_console_message = move(on_console_message);
}

void Application::stop_listening_for_console_messages(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_console_message = nullptr;
}

void Application::listen_for_network_events(DevTools::TabDescription const& description, OnNetworkRequestStarted on_request_started, OnNetworkResponseHeadersReceived on_response_headers, OnNetworkResponseBodyReceived on_response_body, OnNetworkRequestFinished on_request_finished) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_network_request_started = [on_request_started = move(on_request_started)](u64 request_id, URL::URL const& url, ByteString const& method, Vector<HTTP::Header> const& headers, ByteBuffer request_body, Optional<String> initiator_type) {
        on_request_started({ request_id, url.to_string(), MUST(String::from_byte_string(method)), UnixDateTime::now(), headers, move(request_body), move(initiator_type) });
    };

    view->on_network_response_headers_received = [on_response_headers = move(on_response_headers)](u64 request_id, u32 status_code, Optional<String> const& reason_phrase, Vector<HTTP::Header> const& headers) {
        on_response_headers({ request_id, status_code, reason_phrase, headers });
    };

    view->on_network_response_body_received = [on_response_body = move(on_response_body)](u64 request_id, ByteBuffer data) {
        on_response_body(request_id, move(data));
    };

    view->on_network_request_finished = [on_request_finished = move(on_request_finished)](u64 request_id, u64 body_size, Requests::RequestTimingInfo const& timing_info, Optional<Requests::NetworkError> const& network_error) {
        on_request_finished({ request_id, body_size, timing_info, network_error });
    };
}

void Application::stop_listening_for_network_events(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_network_request_started = nullptr;
    view->on_network_response_headers_received = nullptr;
    view->on_network_response_body_received = nullptr;
    view->on_network_request_finished = nullptr;
}

void Application::listen_for_navigation_events(DevTools::TabDescription const& description, OnNavigationStarted on_started, OnNavigationFinished on_finished) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    ViewImplementation::NavigationListener listener;
    listener.on_load_start = [on_started = move(on_started)](URL::URL const& url, bool) {
        on_started(url.to_string());
    };
    listener.on_load_finish = [view_id = view->view_id(), on_finished = move(on_finished)](URL::URL const& url) {
        auto view = ViewImplementation::find_view_by_id(view_id);
        if (!view.has_value())
            return;
        on_finished(url.to_string(), view->title().to_well_formed_utf8());
    };

    auto listener_id = view->add_navigation_listener(move(listener));
    m_navigation_listener_ids.set(description.id, listener_id);
}

void Application::stop_listening_for_navigation_events(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    if (auto listener_id = m_navigation_listener_ids.get(description.id); listener_id.has_value()) {
        view->remove_navigation_listener(listener_id.value());
        m_navigation_listener_ids.remove(description.id);
    }
}

void Application::did_connect_devtools_client(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->did_connect_devtools_client();
}

void Application::did_disconnect_devtools_client(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->did_disconnect_devtools_client();
}

}
