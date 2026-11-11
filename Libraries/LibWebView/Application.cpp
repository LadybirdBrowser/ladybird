/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Checked.h>
#include <AK/Debug.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Math.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/StatsLog.h>
#include <LibCore/System.h>
#include <LibCore/TimeZoneWatcher.h>
#include <LibCore/Timer.h>
#include <LibDatabase/Database.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibDevTools/FirefoxClient.h>
#include <LibDevTools/StorageHelpers.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibHTTP/HeaderList.h>
#include <LibIPC/TransportHandle.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestControlClient.h>
#include <LibURL/InternalURLs.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/CSS/PropertyList.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWebCommon/Loader/DownloadFilename.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/WebDriver/TimeoutsConfiguration.h>
#include <LibWebCommon/WebView/SiteIsolation.h>
#include <LibWebView/Application.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/CompositorClient.h>
#include <LibWebView/CompositorPresentationClient.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/CrashReport.h>
#include <LibWebView/CrashReportOutbox.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/FontService.h>
#include <LibWebView/FontServiceHost.h>
#include <LibWebView/HSTSStore.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/ImageDecoderManager.h>
#include <LibWebView/NoOpMicrophoneAuthorizer.h>
#include <LibWebView/NoOpNotificationBackend.h>
#include <LibWebView/PageTranslation.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/PictureInPictureManager.h>
#include <LibWebView/PictureInPictureWindow.h>
#include <LibWebView/ProcessType.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/SiteCompatibility.h>
#include <LibWebView/SpellChecker.h>
#include <LibWebView/StorageAreaRegistry.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/Translator.h>
#include <LibWebView/URL.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebDriverBiDi.h>
#include <LibWebView/WebDriverBrowserConnection.h>
#include <LibWebView/WorkerAgentManager.h>

#if !defined(AK_OS_WINDOWS)
#    include <LibWebView/TerminalWebView.h>
#endif

#if defined(AK_OS_MACOS)
#    include <LibIPC/MachBootstrapListener.h>
#    include <LibIPC/Transport.h>
#    include <LibIPC/TransportBootstrapMach.h>
#endif

#if defined(AK_OS_WINDOWS)
#    include <LibIPC/HandleBrokerServer.h>
#endif

#ifdef USE_DIRECTX
#    include <LibGfx/Direct3DContext.h>
#endif

#if !defined(AK_OS_WINDOWS)
#    include <signal.h>
#    include <sys/wait.h>
#endif

namespace WebView {

static constexpr size_t maximum_content_blocker_list_size = 64 * MiB;
static constexpr size_t maximum_combined_content_blocker_list_size = 256 * MiB;

Application* Application::s_the = nullptr;

static constexpr double default_display_refresh_rate = 60.0;

static double sanitized_display_refresh_rate(double refresh_rate)
{
    if (refresh_rate != refresh_rate || refresh_rate <= 0 || refresh_rate >= AK::Infinity<double>)
        return default_display_refresh_rate;
    return refresh_rate;
}

struct ApplicationSettingsObserver final : public SettingsObserver {
    AK_ALLOC_WITH_KMALLOC;

    virtual void appearance_changed() override
    {
        Application::the().appearance_changed({});
    }

    virtual void tab_settings_changed() override
    {
        Application::the().tab_settings_changed({});
    }

    virtual void languages_changed() override
    {
        Application::the().languages_changed({});
    }

    virtual void dns_settings_changed() override
    {
        Application::request_server_manager().dns_settings_changed();
    }

    virtual void background_networking_settings_changed() override
    {
        Application::the().background_networking_settings_changed({});
    }

    virtual void content_blocker_settings_changed() override
    {
        Application::the().content_blocker_settings_changed({});
    }

    virtual void permission_settings_changed(Web::Permissions::PermissionName) override
    {
        Application::the().permission_settings_changed({});
    }

    virtual void config_variable_changed(ConfigVariableID id) override
    {
        if (id == ConfigVariableID::MaximumDiskCacheSize)
            Application::request_server_manager().disk_cache_settings_changed();
    }
};

struct ApplicationPermissionManagerObserver final : public PermissionManagerObserver {
    AK_ALLOC_WITH_KMALLOC;

    virtual void permissions_changed(PermissionManager const& manager) override
    {
        Application::the().permissions_changed({}, manager);
    }
};

Application::Application(Optional<ByteString> ladybird_binary_path)
    : m_site_compatibility_data(JsonArray {})
{
    VERIFY(!s_the);
    s_the = this;
    m_ui_process_cross_process_id_allocator = allocate_cross_process_id_allocator();

    platform_init(move(ladybird_binary_path));
}

Application::~Application()
{
#if !defined(AK_OS_WINDOWS)
    // No event loop runs the handlers during teardown, so a signal from here on takes its default action again.
    for (auto handler : m_session_signal_handlers)
        Core::EventLoop::unregister_signal(handler);
#endif

#if defined(AK_OS_MACOS)
    // Stop bootstrap callbacks before destroying the state they use.
    m_mach_port_server.clear();
#endif

    if (m_compositor_client)
        m_compositor_client->on_death = nullptr;
    if (m_compositor_presentation_client)
        m_compositor_presentation_client->on_death = nullptr;

    if (m_cpu_profiler_process.has_value()) {
#if !defined(AK_OS_WINDOWS)
        for (auto handler : m_cpu_profiler_signal_handlers)
            Core::EventLoop::unregister_signal(handler);
        auto wait_result = Core::System::waitpid(m_cpu_profiler_process->pid(), WNOHANG);
        while (wait_result.is_error() && wait_result.error().code() == EINTR)
            wait_result = Core::System::waitpid(m_cpu_profiler_process->pid(), WNOHANG);
        if (!wait_result.is_error() && wait_result.value().pid == 0) {
            if (auto result = Core::System::kill(m_cpu_profiler_process->pid(), SIGINT); result.is_error())
                warnln("Unable to stop CPU profiler: {}", result.error());
            if (auto result = m_cpu_profiler_process->wait_for_termination(); result.is_error())
                warnln("Unable to wait for CPU profiler: {}", result.error());
        } else if (wait_result.is_error() && wait_result.error().code() != ECHILD) {
            warnln("Unable to query CPU profiler: {}", wait_result.error());
        }
#endif
        m_cpu_profiler_process.clear();
        m_cpu_profiler_control_socket.clear();
    }

    m_spare_web_content_process = nullptr;

    PictureInPictureManager::the().destroy_all_windows();

    // Connection die() handlers use the process manager, so destroy the processes while it is still reachable.
    if (m_process_manager)
        m_process_manager->remove_all_processes();
    m_process_manager = nullptr;
    m_browser_process = nullptr;

    s_the = nullptr;
}

void Application::quit_for_signal()
{
    m_event_loop->quit(0);
}

bool Application::claim_cpu_profiler(ProcessType process_type)
{
    if (m_cpu_profiler_claimed || m_browser_options.profile_tool != ProfileTool::CPU || m_browser_options.profile_helper_process != process_type)
        return false;
    m_cpu_profiler_claimed = true;
    return true;
}

void Application::set_cpu_profiler_process(Core::Process process, OwnPtr<Core::File> control_socket)
{
    VERIFY(!m_cpu_profiler_process.has_value());
    m_cpu_profiler_process = move(process);
    m_cpu_profiler_control_socket = move(control_socket);
#if !defined(AK_OS_WINDOWS)
    m_cpu_profiler_signal_handlers.append(Core::EventLoop::register_signal(SIGINT, [this](int) { m_event_loop->quit(0); }));
    m_cpu_profiler_signal_handlers.append(Core::EventLoop::register_signal(SIGTERM, [this](int) { m_event_loop->quit(0); }));
#endif
}

ErrorOr<NonnullRefPtr<Requests::RequestClient>> Application::request_server_client(IsPrivate is_private)
{
    auto instance = TRY(request_server_manager().browser_instance(session_for_new_view(is_private)));
    return instance->ui_client();
}

Requests::RequestControlClient& Application::request_server_control_client()
{
    auto instance = request_server_manager().browser_instance(default_session());
    if (instance.is_error()) {
        warnln("\033[31;1mUnable to launch RequestServer: {}\033[0m", instance.error());
        VERIFY_NOT_REACHED();
    }
    return instance.value()->control_client();
}

ErrorOr<void> Application::initialize(Main::Arguments const& arguments)
{
    TRY(handle_attached_debugger());
    if (auto result = reload_site_compatibility_data(); result.is_error())
        warnln("\033[31;1mUnable to load site compatibility data:\033[0m {}", result.error());
    m_arguments = arguments;

    Vector<ByteString> raw_urls;
    Vector<ByteString> certificates;
    Optional<HeadlessMode> headless_mode;
    Optional<int> window_width;
    Optional<int> window_height;
    Optional<double> device_pixel_ratio;
    Optional<u32> screenshot_delay;
    Optional<StringView> screenshot_path;
    bool new_window = false;
    bool force_new_process = false;
    StringView stats_log_directory;
    Optional<StringView> profile_name;
    Optional<StringView> profile_path;
    bool temporary_profile = false;
    bool allow_popups = false;
    bool disable_scripting = false;
    bool disable_sql_database = false;
    Optional<u16> devtools_port;
    Vector<StringView> debug_processes;
    Optional<StringView> profile_process;
    Optional<StringView> profile_tool;
    Optional<StringView> profile_output;
    Optional<StringView> webdriver_browser_endpoint;
    Optional<StringView> user_agent_preset;
    Optional<StringView> dns_server_address;
    Optional<StringView> default_time_zone;
    Optional<StringView> proxy_autoconfig_url;
    Optional<u16> dns_server_port;
    bool use_dns_over_tls = false;
    bool enable_test_mode = false;
    bool validate_dnssec_locally = false;
    bool log_all_js_exceptions = false;
    auto site_isolation_mode = default_site_isolation_mode();
    bool disable_same_origin_tab_sharing = false;
    Optional<size_t> maximum_site_request_servers;
    bool disable_http_memory_cache = false;
    bool disable_http_disk_cache = false;
    bool disable_content_blocker = false;
    bool disable_sandbox = false;
    bool force_video_decoder_server = false;
    Vector<ByteString> content_blocker_list_paths;
    Optional<StringView> resource_substitution_map_path;
    bool enable_autoplay = false;
    bool expose_experimental_interfaces = false;
    bool expose_internals_object = false;
    bool force_cpu_painting = false;
    bool force_fontconfig = false;
    bool collect_garbage_on_every_allocation = false;
    bool disable_scrollbar_painting = false;
    bool file_scheme_urls_have_tuple_origins = false;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("The Ladybird web browser :^)");
    args_parser.add_positional_argument(raw_urls, "URLs to open", "url", Core::ArgsParser::Required::No);

    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Optional,
        .help_string = "Run Ladybird without a browser window. Mode may be 'screenshot' (default), 'layout-tree', 'text', 'manual', or 'terminal'.",
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
#if !defined(AK_OS_WINDOWS)
            else if (value.equals_ignoring_ascii_case("terminal"sv))
                headless_mode = HeadlessMode::Terminal;
#endif

            return headless_mode.has_value();
        },
    });

    args_parser.add_option(screenshot_delay, "Set the number of seconds to wait before taking a screenshot (only supported for headless screenshot mode)", "screenshot-delay", 0, "seconds");
    args_parser.add_option(screenshot_path, "Save screenshots to the given location (only supported for headless screenshot mode)", "screenshot-path", 0, "path");
    args_parser.add_option(window_width, "Set viewport width in pixels (default: 800) (currently only supported for headless mode)", "window-width", 0, "pixels");
    args_parser.add_option(window_height, "Set viewport height in pixels (default: 600) (currently only supported for headless mode)", "window-height", 0, "pixels");
    args_parser.add_option(device_pixel_ratio, "Set the device pixel ratio (default: 1) (only supported for headless terminal mode)", "device-pixel-ratio", 0, "ratio");
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "URL of a PAC file (not supported yet, requests fail)",
        .long_name = "proxy-autoconfig-url",
        .value_name = "url",
        .accept_value = [&](StringView value) {
            if (!URL::Parser::basic_parse(value).has_value())
                return false;
            proxy_autoconfig_url = value;
            return true;
        },
        .hide_mode = Core::ArgsParser::OptionHideMode::CommandLineAndMarkdown,
    });
    args_parser.add_option(new_window, "Force opening in a new window", "new-window", 'n');
#if !defined(AK_OS_ANDROID)
    args_parser.add_option(force_new_process, "Force creation of a new browser process", "force-new-process");
    args_parser.add_option(stats_log_directory, "Directory for process statistics (JSON lines)", "stats-log-dir", 0, "path");
    args_parser.add_option(profile_name, "Select or create a named profile", "profile", 0, "name");
    args_parser.add_option(profile_path, "Use a self-contained profile at an absolute path", "profile-path", 0, "path");
    args_parser.add_option(temporary_profile, "Use a temporary profile that is removed on clean shutdown", "temporary-profile");
#endif
    args_parser.add_option(allow_popups, "Disable popup blocking by default", "allow-popups");
    args_parser.add_option(disable_scripting, "Disable scripting by default", "disable-scripting");
    args_parser.add_option(disable_sql_database, "Disable SQL database", "disable-sql-database");
    args_parser.add_option(file_scheme_urls_have_tuple_origins, "Treat file:// URLs as having tuple origins", "tuple-file-origins");
    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Wait for a debugger to attach to the given process name (WebContent, RequestServer, etc.)",
        .long_name = "debug-process",
        .value_name = "process-name",
        .accept_value = [&](StringView value) {
            if (value.is_empty())
                return false;

            debug_processes.append(value);
            return true;
        } });
    args_parser.add_option(profile_process, "Profile the given process name (WebContent, RequestServer, etc.)", "profile-process", 0, "process-name");
    args_parser.add_option(profile_tool, "Select the profiler to use: 'callgrind' (default) or 'cpu'", "profile-tool", 0, "tool");
    args_parser.add_option(profile_output, "Write CPU profiler output to the given path", "profile-output", 0, "path");
#if defined(AK_OS_MACOS)
    args_parser.add_option(webdriver_browser_endpoint, "Mach server name for the browser's WebDriver IPC", "webdriver-browser-mach-server-name", 0, "name", Core::ArgsParser::OptionHideMode::CommandLineAndMarkdown);
#else
    args_parser.add_option(webdriver_browser_endpoint, "Path to WebDriver IPC for the browser", "webdriver-browser-path", 0, "path", Core::ArgsParser::OptionHideMode::CommandLineAndMarkdown);
#endif
    args_parser.add_option(enable_test_mode, "Enable test mode", "test-mode");
    args_parser.add_option(log_all_js_exceptions, "Log all JavaScript exceptions", "log-all-js-exceptions");
    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Set site isolation mode. Mode may be 'disable', 'top-level', or 'iframe'.",
        .long_name = "site-isolation",
        .value_name = "mode",
        .accept_value = [&](StringView value) {
            auto parsed_mode = site_isolation_mode_from_string(value);
            if (!parsed_mode.has_value())
                return false;

            site_isolation_mode = *parsed_mode;
            return true;
        },
    });
    args_parser.add_option(disable_same_origin_tab_sharing, "Disable hosting tabs of the same origin in one WebContent process", "disable-same-origin-tab-sharing");
    args_parser.add_option(disable_http_memory_cache, "Disable HTTP memory cache", "disable-http-memory-cache");
    args_parser.add_option(disable_http_disk_cache, "Disable HTTP disk cache", "disable-http-disk-cache");
    args_parser.add_option(maximum_site_request_servers, "How many RequestServers a session may have for the sites it visits, 0 to share one", "max-site-request-servers", 0, "count");
    args_parser.add_option(disable_content_blocker, "Disable content blocker", "disable-content-blocker");
    args_parser.add_option(disable_sandbox, "Disable helper process sandboxing", "disable-sandbox");
    args_parser.add_option(force_video_decoder_server, "Also decode video that has no hardware decoder in the VideoDecoderServer", "force-video-decoder-server");
    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Path to a content blocker list. May be specified multiple times.",
        .long_name = "content-blocker-list",
        .value_name = "path",
        .accept_value = [&](ByteString value) {
            if (value.is_empty())
                return false;

            content_blocker_list_paths.append(move(value));
            return true;
        },
    });
    args_parser.add_option(enable_autoplay, "Enable multimedia autoplay", "enable-autoplay");
    args_parser.add_option(expose_experimental_interfaces, "Expose experimental IDL interfaces", "expose-experimental-interfaces");
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

    m_stats_log = TRY(Core::StatsLog::create(stats_log_directory, "Ladybird"sv));

    if (!dns_server_port.has_value())
        dns_server_port = use_dns_over_tls ? 853 : 53;

    Vector<ProcessType> debug_process_types;
    Optional<ProcessType> profile_process_type;
    auto selected_profile_tool = ProfileTool::Callgrind;

    for (auto& process_name : debug_processes) {
        auto type = process_type_from_name(process_name);
        debug_process_types.append(type);
    }
    if (profile_process.has_value())
        profile_process_type = process_type_from_name(*profile_process);
    if (profile_tool.has_value()) {
        if (*profile_tool == "callgrind"sv)
            selected_profile_tool = ProfileTool::Callgrind;
        else if (*profile_tool == "cpu"sv)
            selected_profile_tool = ProfileTool::CPU;
        else
            return Error::from_string_literal("--profile-tool must be 'callgrind' or 'cpu'");
    }
    if (profile_tool.has_value() && !profile_process.has_value())
        return Error::from_string_literal("--profile-tool requires --profile-process");
    if (profile_output.has_value() && selected_profile_tool != ProfileTool::CPU)
        return Error::from_string_literal("--profile-output requires --profile-tool cpu");

    m_explicit_content_blocker_list_paths = move(content_blocker_list_paths);

    // Disable site isolation when debugging WebContent. Otherwise, the process swap may interfere with the gdb session.
    if (debug_process_types.contains_slow(ProcessType::WebContent))
        site_isolation_mode = SiteIsolationMode::Disabled;

    m_browser_options = {
        .urls = {},
        .raw_urls = move(raw_urls),
        .headless_mode = headless_mode,
        .new_window = new_window ? NewWindow::Yes : NewWindow::No,
        .allow_popups = allow_popups ? AllowPopups::Yes : AllowPopups::No,
        .disable_scripting = disable_scripting ? DisableScripting::Yes : DisableScripting::No,
        .disable_sql_database = disable_sql_database ? DisableSQLDatabase::Yes : DisableSQLDatabase::No,
        .debug_helper_processes = move(debug_process_types),
        .profile_helper_process = move(profile_process_type),
        .profile_tool = selected_profile_tool,
        .profile_output = profile_output.has_value() ? Optional<ByteString> { *profile_output } : OptionalNone {},
        .dns_settings = (dns_server_address.has_value()
                ? Optional<DNSSettings> { use_dns_over_tls
                          ? DNSSettings(DNSOverTLS(dns_server_address.release_value(), *dns_server_port, validate_dnssec_locally))
                          : DNSSettings(DNSOverUDP(dns_server_address.release_value(), *dns_server_port, validate_dnssec_locally)) }
                : OptionalNone()),
        .devtools_port = devtools_port,
        .enable_content_blocker = disable_content_blocker ? EnableContentBlocker::No : EnableContentBlocker::Yes,
        .disable_sandbox = disable_sandbox ? DisableSandbox::Yes : DisableSandbox::No,
        .force_video_decoder_server = force_video_decoder_server ? ForceVideoDecoderServer::Yes : ForceVideoDecoderServer::No,
    };

    if (screenshot_delay.has_value())
        m_browser_options.screenshot_delay = *screenshot_delay;
    if (screenshot_path.has_value())
        m_browser_options.screenshot_path = *screenshot_path;
    if (window_width.has_value())
        m_browser_options.window_width = *window_width;
    if (window_height.has_value())
        m_browser_options.window_height = *window_height;
    if (device_pixel_ratio.has_value())
        m_browser_options.device_pixel_ratio = *device_pixel_ratio;

#if !defined(AK_OS_WINDOWS)
    if (headless_mode == HeadlessMode::Terminal)
        TRY(TerminalWebView::take_over_terminal());
#endif

    if (webdriver_browser_endpoint.has_value())
        m_browser_options.webdriver_browser_endpoint = *webdriver_browser_endpoint;

    auto http_disk_cache_mode = HTTPDiskCacheMode::Enabled;
    if (disable_http_disk_cache)
        http_disk_cache_mode = HTTPDiskCacheMode::Disabled;

    m_request_server_options = {
        .certificates = move(certificates),
        .cache_path = {},
        .http_disk_cache_mode = http_disk_cache_mode,
        .resource_substitution_map_path = resource_substitution_map_path.has_value() ? Optional<ByteString> { *resource_substitution_map_path } : OptionalNone {},
        .proxy_configuration = HTTP::ProxyConfiguration::from_environment(),
        .maximum_site_request_servers = maximum_site_request_servers.value_or(RequestServerOptions {}.maximum_site_request_servers),
    };

    m_web_content_options = {
        .user_agent_preset = move(user_agent_preset),
        .is_test_mode = enable_test_mode ? IsTestMode::Yes : IsTestMode::No,
        .log_all_js_exceptions = log_all_js_exceptions ? LogAllJSExceptions::Yes : LogAllJSExceptions::No,
        .site_isolation_mode = site_isolation_mode,
        .share_process_between_same_origin_tabs = disable_same_origin_tab_sharing ? ShareProcessBetweenSameOriginTabs::No : ShareProcessBetweenSameOriginTabs::Yes,
        .enable_http_memory_cache = disable_http_memory_cache ? EnableMemoryHTTPCache::No : EnableMemoryHTTPCache::Yes,
        .expose_experimental_interfaces = expose_experimental_interfaces ? ExposeExperimentalInterfaces::Yes : ExposeExperimentalInterfaces::No,
        .expose_internals_object = expose_internals_object ? ExposeInternalsObject::Yes : ExposeInternalsObject::No,
        .force_cpu_painting = force_cpu_painting ? ForceCPUPainting::Yes : ForceCPUPainting::No,
        .force_fontconfig = force_fontconfig ? ForceFontconfig::Yes : ForceFontconfig::No,
        .enable_autoplay = enable_autoplay ? EnableAutoplay::Yes : EnableAutoplay::No,
        .collect_garbage_on_every_allocation = collect_garbage_on_every_allocation ? CollectGarbageOnEveryAllocation::Yes : CollectGarbageOnEveryAllocation::No,
        .paint_viewport_scrollbars = disable_scrollbar_painting ? PaintViewportScrollbars::No : PaintViewportScrollbars::Yes,
        .file_scheme_urls_have_tuple_origins = file_scheme_urls_have_tuple_origins ? FileSchemeUrlsHaveTupleOrigins::Yes : FileSchemeUrlsHaveTupleOrigins::No,
        .default_time_zone = default_time_zone,
        .proxy_autoconfig_url = proxy_autoconfig_url.map([](auto value) { return value.to_byte_string(); }),
    };

    create_platform_options(m_browser_options, m_request_server_options, m_web_content_options);
    TRY(create_profile(profile_name, profile_path, force_new_process, temporary_profile));

    // Browsers launched by WebDriver are driven directly and must never join an existing browser process. A browser
    // that logs process statistics must not join one either, since that browser would not sample them.
    bool coordinate_browser_process = !headless_mode.has_value() && !webdriver_browser_endpoint.has_value() && !m_stats_log && should_coordinate_browser_process();

    // Synchronous IPC used to forward URLs to an existing browser process requires an event loop.
    if (coordinate_browser_process)
        m_event_loop = &create_platform_event_loop();

    auto profile_identity = Profile::routing_identifier(profile().paths().identity);
    create_mach_port_server(profile_identity);

    m_browser_process = make<BrowserProcess>();
    if (coordinate_browser_process) {
        auto disposition = TRY(m_browser_process->connect(m_browser_options.raw_urls, m_browser_options.new_window, profile().paths().runtime, profile_identity));
        if (disposition == BrowserProcess::ProcessDisposition::ExitProcess) {
            m_should_exit_after_profile_coordination = true;
            return {};
        }
    }

    // Reports belong to the profile they came from, whose own setting decides what happens to them. A temporary
    // profile takes nothing over, since it would take it to its grave.
    CrashReport::set_sandbox_is_disabled(m_browser_options.disable_sandbox);
    auto& crash_report_store = CrashReportStore::the();
    crash_report_store.set_directory(LexicalPath::join(profile().paths().data, "CrashReports"sv).string());
    if (!profile().is_temporary()) {
        if (auto result = crash_report_store.adopt_reports_from(CrashReportStore::default_directory()); result.is_error())
            warnln("Could not move crash reports into the profile: {}", result.error());
    }
    if (captures_browser_crashes()) {
        if (auto result = crash_report_store.initialize_browser_crash_handler(); result.is_error())
            warnln("Could not prepare Browser crash reporting: {}", result.error());
    }

    // Test results must not depend on the languages of the machine running the tests.
    auto platform_languages = m_web_content_options.is_test_mode == IsTestMode::Yes ? Vector<String> {} : platform_preferred_languages();

    m_settings = make<Settings>(Settings::create(LexicalPath::join(profile().paths().config, "Settings.json"sv).string(), move(platform_languages)));
    // A browser that crashed left a report that was recovered before the settings were read.
    if (m_settings->crash_report_sending() == CrashReportSending::NeverSend)
        CrashReportOutbox::the().discard_all();

    if (m_web_content_options.is_test_mode == IsTestMode::Yes) {
        TRY(m_settings->complete_first_run());
        m_settings->set_background_networking_enabled(false);
        m_settings->set_content_blocker_enabled(true);
    }

    m_browser_options.urls = sanitize_urls(m_browser_options.raw_urls);
    m_request_server_options.cache_path = profile().paths().cache;
    m_web_content_options.cache_path = profile().paths().cache;

    m_content_blocker_lists_directory = LexicalPath::join(profile().paths().data, "ContentBlocking/Lists"sv).string();
    rebuild_content_blocker_list_paths();

    if (!m_settings->first_run_complete() && !m_browser_options.headless_mode.has_value()
        && !m_browser_options.webdriver_browser_endpoint.has_value()) {
        if (m_browser_options.raw_urls.is_empty())
            m_browser_options.urls = { URL::about_welcome() };
        else if (!m_browser_options.urls.contains_slow(URL::about_welcome()))
            m_browser_options.urls.prepend(URL::about_welcome());
    }

    m_font_service = FontService::create(m_browser_options.additional_font_directories);

    // Test mode implies experimental interfaces and internals object are exposed and the Skia CPU backend is used.
    if (m_web_content_options.is_test_mode == IsTestMode::Yes) {
        m_web_content_options.expose_experimental_interfaces = ExposeExperimentalInterfaces::Yes;
        m_web_content_options.expose_internals_object = ExposeInternalsObject::Yes;
        m_web_content_options.force_cpu_painting = ForceCPUPainting::Yes;
    }

    if (m_web_content_options.file_scheme_urls_have_tuple_origins == FileSchemeUrlsHaveTupleOrigins::Yes)
        URL::set_file_scheme_urls_have_tuple_origins();

    set_site_isolation_mode(m_web_content_options.site_isolation_mode);

    if (auto result = load_content_blocker_lists(); result.is_error()) {
        warnln("\033[31;1mUnable to load all content blocker lists:\033[0m {}", result.error());
        warnln("    Configured lists: {}", m_browser_options.content_blocker_list_paths);
    }

    if (!m_event_loop)
        m_event_loop = &create_platform_event_loop();
    if (captures_browser_crashes())
        CrashReportOutbox::the().start_removing_expired_reports();

    // NB: The host's thread makes an event loop, which would install the default event loop manager if the platform
    //     one were not installed yet.
    m_font_service_host = FontServiceHost::create(*m_font_service);
    if (m_stats_log)
        m_stats_log->start_sampling();
    TRY(launch_services());

    if (m_browser_options.enable_content_blocker == EnableContentBlocker::Yes) {
        m_content_blocker_list_update_timer = Core::Timer::create_repeating(AK::Duration::from_seconds(24 * 60 * 60), [this] {
            start_content_blocker_list_update(ContentBlockerListUpdateTrigger::Automatic);
        });
        m_content_blocker_list_update_timer->start();
        start_content_blocker_list_update(ContentBlockerListUpdateTrigger::Automatic);
    }

    initialize_page_options();

    return {};
}

ErrorOr<void> Application::create_profile([[maybe_unused]] Optional<StringView> profile_name, [[maybe_unused]] Optional<StringView> profile_path, [[maybe_unused]] bool force_new_process, [[maybe_unused]] bool temporary_profile)
{
#if !defined(AK_OS_ANDROID)
    ProfileSelection profile_selection { .temporary = temporary_profile };

    if (force_new_process) {
        if (profile_name.has_value() || profile_path.has_value() || temporary_profile)
            return Error::from_string_literal("--force-new-process cannot be combined with another profile selector");
        warnln("--force-new-process is deprecated; using --temporary-profile");
        profile_selection.temporary = true;
    }

    if (profile_name.has_value())
        profile_selection.name = profile_name->to_byte_string();
    if (profile_path.has_value())
        profile_selection.path = profile_path->to_byte_string();

    if (m_web_content_options.is_test_mode == IsTestMode::Yes) {
        if (profile_name.has_value() || profile_path.has_value())
            return Error::from_string_literal("Test mode cannot be combined with a persistent profile selector");
        profile_selection.temporary = true;
    }
#endif

    ProfileRoots profile_roots {
        .config = Core::StandardPaths::config_directory(),
        .data = Core::StandardPaths::user_data_directory(),
        .cache = Core::StandardPaths::cache_directory(),
        .runtime = TRY(Core::StandardPaths::runtime_directory()),
        .temporary = Core::StandardPaths::tempfile_directory(),
    };
#if defined(AK_OS_WINDOWS)
    // These StandardPaths values already contain "Ladybird" on Windows. Use their parents as the platform roots for profile layouts.
    profile_roots.data = LexicalPath { profile_roots.data }.parent().string();
    profile_roots.cache = LexicalPath { profile_roots.cache }.parent().string();
#endif

#if defined(AK_OS_ANDROID)
    m_profile = TRY(Profile::create_legacy(profile_roots));
#else
    m_profile = TRY(Profile::create(profile_selection, profile_roots));
#endif

    return {};
}

void Application::create_mach_port_server([[maybe_unused]] ByteString const& profile_identity)
{
#if defined(AK_OS_MACOS)
    auto mach_process_name = ByteString::formatted("Ladybird-{}", profile_identity);
    m_mach_port_server = make<IPC::MachBootstrapListener>(mach_server_name_for_process(mach_process_name, Core::System::getpid()));
    set_mach_server_name(m_mach_port_server->server_port_name());

    m_mach_port_server->on_bootstrap_request = [this](IPC::MachBootstrapListener::BootstrapRequest request) {
        auto result = m_transport_bootstrap_server.handle_bootstrap_request(request.pid, move(request.reply_port));
        if (result.is_error()) {
            dbgln("Unable to bootstrap helper process {}: {}", request.pid, result.error());
            return;
        }
        result.release_value().visit(
            [this, pid = request.pid, task_name_port = move(request.task_name_port)](IPC::TransportBootstrapMachServer::ChildTransportHandled) mutable {
                // The child sends its task name port before Process::spawn() returns, so it cannot be added to the process
                // manager yet. Install the port once control returns to the browser event loop.
                VERIFY(m_event_loop);
                m_event_loop->deferred_invoke([this, pid, task_name_port = move(task_name_port)]() mutable {
                    set_process_task_name_port(pid, move(task_name_port));
                });
            },
            [this](IPC::TransportBootstrapMachServer::OnDemandTransport& transport) {
                if (!m_on_browser_process_transport)
                    return;

                VERIFY(m_event_loop);
                m_event_loop->deferred_invoke([this, transport = move(transport.ports)]() mutable {
                    if (!m_on_browser_process_transport)
                        return;
                    m_on_browser_process_transport(make<IPC::Transport>(move(transport.receive_right), move(transport.send_right)));
                });
            });
    };
#endif
}

ByteString Application::content_blocker_list_path(StringView identifier) const
{
    return LexicalPath::join(m_content_blocker_lists_directory, ByteString::formatted("{}.txt", identifier)).string();
}

Optional<UnixDateTime> Application::content_blocker_list_last_updated_at(StringView identifier) const
{
    if (auto file_status = Core::File::stat(content_blocker_list_path(identifier)); !file_status.is_error())
        return UnixDateTime::from_seconds_since_epoch(file_status.value().st_mtime);
    return {};
}

void Application::rebuild_content_blocker_list_paths()
{
    m_browser_options.content_blocker_list_paths = m_explicit_content_blocker_list_paths;

    for (auto const& list : m_settings->content_blocker_lists()) {
        auto path = content_blocker_list_path(list.identifier);
        if (list.enabled && FileSystem::is_regular_file(path))
            m_browser_options.content_blocker_list_paths.append(move(path));
    }
}

ErrorOr<bool> Application::load_content_blocker_lists()
{
    auto custom_filters = m_settings->custom_content_blocker_filters();
    if (m_browser_options.content_blocker_list_paths.is_empty() && custom_filters.is_empty()) {
        if (m_content_blocker_list_buffer.has_value() && m_content_blocker_list_buffer->size() == 0)
            return false;
        m_content_blocker_list_buffer = TRY(Core::AnonymousBuffer::create_with_size(0));
        return true;
    }

    Checked<size_t> total_size = 0;
    for (auto const& path : m_browser_options.content_blocker_list_paths) {
        auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
        total_size += TRY(file->size());
        total_size += 1;
    }
    total_size += custom_filters.bytes().size();
    total_size += 1;

    if (total_size.has_overflow() || total_size.value() > maximum_combined_content_blocker_list_size)
        return Error::from_string_literal("Content blocker lists are too large");

    auto blocker_list_buffer = TRY(Core::AnonymousBuffer::create_with_size(total_size.value()));
    auto bytes = Bytes { blocker_list_buffer.data<u8>(), blocker_list_buffer.size() };
    size_t offset = 0;

    for (auto const& path : m_browser_options.content_blocker_list_paths) {
        auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
        auto file_size = TRY(file->size());
        TRY(file->read_until_filled(bytes.slice(offset, file_size)));
        offset += file_size;
        bytes[offset++] = '\n';
    }
    custom_filters.bytes().copy_to(bytes.slice(offset));
    offset += custom_filters.bytes().size();
    bytes[offset++] = '\n';
    VERIFY(offset == bytes.size());

    if (m_content_blocker_list_buffer.has_value() && m_content_blocker_list_buffer->bytes() == blocker_list_buffer.bytes())
        return false;

    m_content_blocker_list_buffer = move(blocker_list_buffer);
    return true;
}

void Application::start_content_blocker_list_update(ContentBlockerListUpdateTrigger trigger, Optional<StringView> requested_identifier)
{
    if (trigger == ContentBlockerListUpdateTrigger::Automatic && !m_settings->automatic_filter_list_updates_allowed())
        return;
    if (content_blocker_list_update_in_progress() && !requested_identifier.has_value())
        return;

    for (auto const& list : m_settings->content_blocker_lists()) {
        if (!list.enabled || !list.url.has_value() || (requested_identifier.has_value() && list.identifier != *requested_identifier))
            continue;
        if (m_active_content_blocker_list_update.has_value() && m_active_content_blocker_list_update->identifier == list.identifier)
            continue;
        if (any_of(m_pending_content_blocker_list_updates, [&](auto const& update) { return update.identifier == list.identifier; }))
            continue;
        auto updated_at = content_blocker_list_last_updated_at(list.identifier);
        if (trigger == ContentBlockerListUpdateTrigger::Automatic && updated_at.has_value() && UnixDateTime::now() - *updated_at < AK::Duration::from_seconds(24 * 60 * 60))
            continue;
        m_pending_content_blocker_list_updates.append({ list.identifier, list.name, *list.url, content_blocker_list_path(list.identifier), trigger });
    }

    if (!m_active_content_blocker_list_update.has_value())
        start_next_content_blocker_list_update();
}

void Application::start_next_content_blocker_list_update()
{
    m_pending_content_blocker_list_updates.remove_all_matching([&](auto const& update) {
        auto list = m_settings->content_blocker_list(update.identifier);
        return !list.has_value() || !list->enabled;
    });
    if (m_pending_content_blocker_list_updates.is_empty()) {
        if (exchange(m_content_blocker_list_update_had_success, false))
            apply_content_blocker_settings();
        return;
    }

    m_active_content_blocker_list_update = m_pending_content_blocker_list_updates.take_first();
    auto& update = *m_active_content_blocker_list_update;

    auto request_headers = HTTP::HeaderList::create();
    request_headers->set({ "User-Agent"sv, Web::default_user_agent });

    auto client = request_server_client();
    if (client.is_error()) {
        warnln("Unable to launch RequestServer: {}", client.error());
        finish_current_content_blocker_list_update();
        return;
    }
    m_content_blocker_list_update_request = client.value()->start_request(
        "GET"sv, update.url, {}, *request_headers, {}, HTTP::CacheMode::NoStore,
        HTTP::Cookie::IncludeCredentials::No);
    if (!m_content_blocker_list_update_request) {
        warnln("Unable to start update request for {}", update.name);
        finish_current_content_blocker_list_update();
        return;
    }

    auto timeout_ms = []() {
        if (auto value = Core::Environment::get("LADYBIRD_CONTENT_BLOCKER_LIST_UPDATE_TIMEOUT_MS"sv); value.has_value()) {
            if (auto parsed = value->to_number<int>(); parsed.has_value() && *parsed > 0)
                return *parsed;
        }

        return 30 * 1000;
    }();

    auto timeout = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(timeout_ms), [this] {
        warnln("Content blocker list download timed out");
        stop_current_content_blocker_list_update_and_continue();
    });
    m_content_blocker_list_update_request->set_stop_callback([timeout] { timeout->stop(); });
    timeout->start();

    m_content_blocker_list_update_request->set_unbuffered_request_callbacks(
        [this](NonnullRefPtr<HTTP::HeaderList> headers, Optional<u32> response_code, Optional<String> const& reason_phrase, Requests::CacheState) {
            if (m_content_blocker_list_update_cancelled)
                return;
            if (response_code.has_value() && Web::Fetch::Infrastructure::is_redirect_status(*response_code)) {
                auto update = *m_active_content_blocker_list_update;
                auto location = headers->get("Location"sv);
                auto url = location.has_value() ? URL::Parser::basic_parse(*location, update.url) : Optional<URL::URL> {};
                if (url.has_value() && (url->scheme() == "https"sv || (url->scheme() == "http"sv && update.url.scheme() == "http"sv)) && update.redirect_count < 20) {
                    update.url = url.release_value();
                    ++update.redirect_count;
                    m_pending_content_blocker_list_updates.prepend(move(update));
                    stop_current_content_blocker_list_update_and_continue();
                    return;
                }
            }
            m_content_blocker_list_update_response_ok = response_code.has_value() && *response_code >= 200 && *response_code < 300;
            if (!m_content_blocker_list_update_response_ok) {
                warnln("Unable to update content blocker list: received HTTP status {} {}", response_code.value_or(0), reason_phrase.value_or(String {}));
                stop_current_content_blocker_list_update_and_continue();
            }
        },
        [this](Requests::ResponseData data) {
            did_receive_content_blocker_list_update_data(data.bytes());
        },
        [this](Core::ImmutableBytes data) {
            did_receive_content_blocker_list_update_data(data.bytes());
        },
        [this, timeout](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> network_error) {
            timeout->stop();
            VERIFY(m_active_content_blocker_list_update.has_value());
            if (network_error.has_value()) {
                warnln("Unable to update content blocker list: {}", Requests::network_error_to_string(*network_error));
            } else if (!m_content_blocker_list_update_cancelled && m_content_blocker_list_update_response_ok) {
                if (auto result = save_content_blocker_list(m_active_content_blocker_list_update->path, m_content_blocker_list_update_payload.bytes()); result.is_error())
                    warnln("Unable to save content blocker list update: {}", result.error());
                else
                    m_content_blocker_list_update_had_success = true;
            }
            finish_current_content_blocker_list_update();
        });
}

void Application::did_receive_content_blocker_list_update_data(ReadonlyBytes data)
{
    if (m_content_blocker_list_update_cancelled)
        return;

    VERIFY(m_content_blocker_list_update_payload.size() <= maximum_content_blocker_list_size);
    if (data.size() > maximum_content_blocker_list_size - m_content_blocker_list_update_payload.size()) {
        warnln("Unable to update content blocker list: response exceeds {} bytes", maximum_content_blocker_list_size);
        stop_current_content_blocker_list_update_and_continue();
        return;
    }

    if (auto result = m_content_blocker_list_update_payload.try_append(data); result.is_error()) {
        warnln("Unable to update content blocker list: {}", result.error());
        stop_current_content_blocker_list_update_and_continue();
    }
}

void Application::stop_current_content_blocker_list_update_and_continue()
{
    if (!m_content_blocker_list_update_request || m_content_blocker_list_update_cancelled)
        return;

    m_content_blocker_list_update_cancelled = true;
    m_content_blocker_list_update_request->stop();
    finish_current_content_blocker_list_update();
}

void Application::finish_current_content_blocker_list_update()
{
    Core::deferred_invoke([this, request = m_content_blocker_list_update_request] {
        if (m_content_blocker_list_update_request != request)
            return;
        m_content_blocker_list_update_request = nullptr;
        m_active_content_blocker_list_update.clear();
        m_content_blocker_list_update_payload.clear();
        m_content_blocker_list_update_response_ok = false;
        m_content_blocker_list_update_cancelled = false;
        start_next_content_blocker_list_update();
    });
}

ErrorOr<void> Application::save_content_blocker_list(ByteString const& path, ReadonlyBytes contents)
{
    TRY(Core::Directory::create(m_content_blocker_lists_directory, Core::Directory::CreateDirectories::Yes));
    auto temporary_path = ByteString::formatted("{}.tmp", path);
    {
        auto file = TRY(Core::File::open(temporary_path, Core::File::OpenMode::Write));
        TRY(file->write_until_depleted(contents));
    }
    return FileSystem::move_file(path, temporary_path);
}

ErrorOr<void> Application::import_local_content_blocker_list(String name, String contents)
{
    if (contents.bytes().size() > maximum_content_blocker_list_size)
        return Error::from_string_literal("Content blocker list exceeds 64 MiB");

    auto identifier = m_settings->add_content_blocker_list(move(name));
    auto result = save_content_blocker_list(content_blocker_list_path(identifier), contents.bytes());
    if (result.is_error()) {
        m_settings->remove_content_blocker_list(identifier);
        return result.release_error();
    }

    apply_content_blocker_settings();
    return {};
}

void Application::remove_content_blocker_list(Badge<SettingsUI>, StringView identifier)
{
    if (!m_settings->remove_content_blocker_list(identifier))
        return;
    m_pending_content_blocker_list_updates.remove_all_matching([&](auto const& update) { return update.identifier == identifier; });
    if (m_active_content_blocker_list_update.has_value() && m_active_content_blocker_list_update->identifier == identifier)
        stop_current_content_blocker_list_update_and_continue();
    (void)FileSystem::remove(content_blocker_list_path(identifier), FileSystem::RecursionMode::Disallowed);
}

void Application::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab, ViewImplementation const* source_view) const
{
    if (auto view = open_blank_new_tab(activate_tab, source_view); view.has_value())
        view->load(url);
}

void Application::read_clipboard_text(ClipboardType type, Function<void(Utf16String)> on_read) const
{
    on_read(clipboard_text(type));
}

void Application::read_clipboard_item(Function<void(Web::Clipboard::SystemClipboardItem)> on_read) const
{
    on_read(clipboard_item());
}

void Application::activate_or_open_tab_with_url(URL::URL const& url) const
{
    if (auto view = activate_tab_with_url(url, URL::ExcludeFragment::Yes); view.has_value())
        view->load(url);
    else
        open_url_in_new_tab(url, Web::HTML::ActivateTab::Yes);
}

void Application::open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab, ViewImplementation const* source_view) const
{
    if (auto view = open_blank_new_tab(activate_tab, source_view); view.has_value())
        view->load(move(navigation));
}

void Application::open_urls_in_new_tabs(ReadonlySpan<URL::URL> urls) const
{
    for (auto const& url : urls)
        open_url_in_new_tab(url, Web::HTML::ActivateTab::No);
}

ErrorOr<NonnullRefPtr<WebContentClient>> Application::create_web_content_client(IsPrivate is_private, Web::PageId initial_page_id, Optional<Web::HTML::CrossProcessId> navigable_to_adopt, Optional<Web::HTML::CrossProcessId> initial_document_state_id, Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigables, Optional<Web::HTML::SessionHistoryEntryDescriptor> canonical_initial_history_entry, Web::HTML::VisibilityState system_visibility_state)
{
    // The client's WebContentClient picks up this same session when it is created.
    auto session = session_for_new_view(is_private);

    auto cross_process_id_allocator = allocate_cross_process_id_allocator();
    auto root_navigable_id = navigable_to_adopt.has_value() ? *navigable_to_adopt : cross_process_id_allocator.allocate();
    if (!initial_document_state_id.has_value())
        initial_document_state_id = cross_process_id_allocator.allocate();
    auto initial_document_id = cross_process_id_allocator.allocate();

    auto client = TRY(WebView::launch_web_content_process(is_private, initial_page_id, root_navigable_id));
    TRY(Application::the().connect_web_content_to_compositor(*client));

    // A tab's first process creates its traversable from this entry. A process hosting a navigable of an existing tab
    // stands in for the canonical current entry.
    auto initial_history_entry = [&] {
        if (canonical_initial_history_entry.has_value())
            return canonical_initial_history_entry.release_value();
        // The UI process determines the origin of the traversable's first document, which has no creator, before the
        // process creates it.
        auto entry = Web::HTML::create_initial_session_history_entry_descriptor(*initial_document_state_id, {}, {});
        entry.document_state.origin = URL::Origin::create_opaque();
        return entry;
    }();
    client->async_initialize(initial_page_id, move(remote_navigables), root_navigable_id, cross_process_id_allocator, initial_history_entry, initial_document_id, system_visibility_state);

    if (!navigable_to_adopt.has_value())
        client->set_initial_top_level_history_entry({}, move(initial_history_entry), initial_document_id);

    TRY(client->request_server_site_bindings().connect(session));
    client->async_set_site_compatibility_data(m_site_compatibility_data);
    // A process learns a site's permissions only when it shows that site and asks; at launch, just the defaults.
    client->async_set_permission_snapshot(client->session().permission_manager->snapshot_for({}));

    // NB: A WebDriver BiDi session gives its state to every process hosting web content when it changes.
    if (auto* webdriver_bidi = WebDriverBiDi::the(); webdriver_bidi && webdriver_bidi->session_state().is_active)
        webdriver_bidi->push_session_state_for_client(*client);

    if (m_content_blocker_list_buffer.has_value())
        client->async_set_content_blockers(*m_content_blocker_list_buffer);

    m_web_content_clients.set(client);
    return client;
}

ErrorOr<void> Application::reload_site_compatibility_data()
{
    auto data = TRY(load_site_compatibility_data());
    m_site_compatibility_data = move(data);

    WebContentClient::for_each_client([&](auto& client) {
        client.async_set_site_compatibility_data(m_site_compatibility_data);
        return IterationDecision::Continue;
    });
    WorkerAgentManager::the().update_site_compatibility_data(m_site_compatibility_data);
    return {};
}

Web::PageId Application::allocate_page_id()
{
    VERIFY(m_next_page_or_compositor_context_id > 0);
    return Web::PageId { m_next_page_or_compositor_context_id++ };
}

Web::HTML::CrossProcessIdAllocator Application::allocate_cross_process_id_allocator()
{
    VERIFY(m_next_cross_process_id_namespace > 0);
    return Web::HTML::CrossProcessIdAllocator { .namespace_id = m_next_cross_process_id_namespace++ };
}

Web::HTML::CrossProcessId Application::allocate_ui_process_cross_process_id()
{
    return m_ui_process_cross_process_id_allocator.allocate();
}

NonnullRefPtr<BrowsingSession> Application::session_for_new_view(IsPrivate is_private)
{
    if (is_private == IsPrivate::No)
        return *the().m_default_session;

    if (auto session = the().m_private_session.strong_ref())
        return *session;

    auto session = BrowsingSession::create(IsPrivate::Yes);
    session->permission_manager = PermissionManager::create_for_private_session(*the().m_default_session->permission_manager);
    session->permission_manager->register_observer(*the().m_permission_manager_observer);
    session->permission_manager->on_deadlines_changed = [] { the().schedule_permission_expiry(); };
    the().m_private_session = session;
    return session;
}

void Application::did_connect_request_server_client(int client_id, BrowsingSession& session, RequestServer::SiteBinding site_binding)
{
    m_request_server_client_sessions.set(client_id, session.make_weak_ptr());
    if (site_binding == RequestServer::SiteBinding::Unrestricted)
        m_unrestricted_request_server_clients.set(client_id);
}

void Application::did_disconnect_request_server_client(int client_id)
{
    m_request_server_client_sessions.remove(client_id);
    m_unrestricted_request_server_clients.remove(client_id);
}

bool Application::request_server_client_may_use_cookies_in(int client_id, URL::URL const& url, Optional<HTTP::Cookie::PartitionContext> const& context) const
{
    if (m_unrestricted_request_server_clients.contains(client_id))
        return true;

    // NB: A bound client makes a request without a context for a document under a top-level document with an opaque
    //     origin. That is a third-party context of no site, which has no cookies.
    if (!context.has_value())
        return false;

    bool may_use_cookies = false;
    auto check_bindings = [&](RequestServerSiteBindings& bindings) {
        if (!bindings.has_client(client_id))
            return IterationDecision::Continue;
        may_use_cookies = bindings.may_use_cookies_under(client_id, context->top_level_site, url);
        return IterationDecision::Break;
    };
    WebContentClient::for_each_client([&](WebContentClient& client) { return check_bindings(client.request_server_site_bindings()); });
    WorkerAgentManager::the().for_each_request_server_site_bindings([&](RequestServerSiteBindings& bindings) { return check_bindings(bindings); });
    return may_use_cookies;
}

Vector<int> Application::request_server_client_ids_for_testing(BrowsingSession const& session) const
{
    Vector<int> client_ids;
    for (auto const& [client_id, client_session] : m_request_server_client_sessions) {
        if (client_session.ptr() == &session)
            client_ids.append(client_id);
    }
    return client_ids;
}

RefPtr<BrowsingSession> Application::session_for_request_server_client(int client_id) const
{
    if (auto it = m_request_server_client_sessions.find(client_id); it != m_request_server_client_sessions.end())
        return it->value.strong_ref();
    return {};
}

RefPtr<BrowsingSession> Application::existing_session(IsPrivate is_private)
{
    return is_private == IsPrivate::Yes ? the().m_private_session.strong_ref() : the().m_default_session;
}

WebDriverBrowserConnection* Application::webdriver_browser_connection()
{
    if (!m_browser_options.webdriver_browser_endpoint.has_value() || m_webdriver_browser_connection_failed)
        return nullptr;

    if (!m_webdriver_browser_connection) {
        auto connection = WebDriverBrowserConnection::connect(*m_browser_options.webdriver_browser_endpoint);
        if (connection.is_error()) {
            warnln("Unable to connect the browser to WebDriver: {}", connection.error());
            m_webdriver_browser_connection_failed = true;
            return nullptr;
        }
        m_webdriver_browser_connection = connection.release_value();
    }

    return m_webdriver_browser_connection.ptr();
}

void Application::notify_webdriver_window_created(String const& handle)
{
    if (auto* connection = webdriver_browser_connection())
        connection->async_did_create_window(handle);
}

void Application::notify_webdriver_window_closed(String const& handle)
{
    if (auto* connection = webdriver_browser_connection())
        connection->async_did_close_window(handle);
}

void Application::webdriver_browser_connection_died(Badge<WebDriverBrowserConnection>)
{
    m_webdriver_browser_connection = nullptr;
    m_webdriver_browser_connection_failed = true;
}

void Application::push_webdriver_session_config(ViewImplementation& view)
{
    view.traversable().for_each_hosting_page([&](WebContentPage& page) {
        push_webdriver_session_config(page);
    });
}

void Application::push_webdriver_session_config(WebContentPage& page)
{
    auto const& config = m_webdriver_session_config;
    page.async_set_webdriver_session_config(config.user_prompt_handler, config.page_load_strategy, config.strict_file_interactability, config.timeouts);
}

void Application::update_webdriver_session_config(Badge<WebDriverBrowserConnection>, Function<void(WebDriverSessionConfig&)> update)
{
    update(m_webdriver_session_config);

    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        push_webdriver_session_config(view);
        return IterationDecision::Continue;
    });
}

Optional<u64> Application::webdriver_page_load_timeout() const
{
    Web::WebDriver::TimeoutsConfiguration timeouts;
    (void)Web::WebDriver::json_deserialize_as_a_timeouts_configuration_into(m_webdriver_session_config.timeouts, timeouts);
    return timeouts.page_load_timeout;
}

void Application::complete_webdriver_content_command(u64 command_id, Web::WebDriver::Response response)
{
    if (auto* webdriver_bidi = WebDriverBiDi::the(); webdriver_bidi && webdriver_bidi->did_complete_content_command(command_id, response))
        return;

    if (m_webdriver_browser_connection)
        m_webdriver_browser_connection->async_command_complete(command_id, move(response));
}

void Application::reset_private_browsing_session()
{
    m_file_downloader.discard_private_downloads();

    // RequestServer answers our own private client with the cookies of the session it was created for, so the next
    // private request made by the UI process needs a client of the new session.

    // NB: Those views own the session until they are deleted, so let go of it here. The private window opened next
    //     starts a new one rather than picking up what this was asked to clear.
    m_private_session.clear();
    WorkerAgentManager::the().remove_private_workers_without_owners();
}

Web::CompositorContextId Application::allocate_compositor_context_id()
{
    VERIFY(m_next_page_or_compositor_context_id > 0);
    return Web::CompositorContextId { m_next_page_or_compositor_context_id++ };
}

template<typename Client>
static bool can_send_compositor_process_ipc(RefPtr<Client> const& compositor_client)
{
    if (!compositor_client)
        return false;
    return compositor_client->is_open();
}

ErrorOr<void> Application::connect_web_content_to_compositor(WebContentClient& web_content_client)
{
    if (web_content_client.compositor_connection_id({}).has_value())
        return {};

    if (!m_compositor_client)
        return Error::from_string_literal("Compositor process is not available");

    auto response_or_error = m_compositor_client->try_connect_web_content();
    if (response_or_error.is_error())
        return Error::from_string_literal("Compositor process disconnected while connecting WebContent");
    auto response = response_or_error.release_value();

    // The render clock channel is made here, so neither process is handed an end of the other's choosing.
    auto render_clock_channel = TRY(IPC::TransportHandle::create_pair());

    web_content_client.set_compositor_connection_id({}, response.web_content_connection_id());
    m_compositor_client->async_connect_render_clock(response.web_content_connection_id(), move(render_clock_channel.first));
    web_content_client.async_connect_to_compositor_process(response.take_handle(), move(render_clock_channel.second));
    return {};
}

bool Application::connect_video_presentation_to_compositor(WebContentClient& web_content_client, IPC::TransportHandle compositor_end)
{
    auto compositor_connection_id = web_content_client.compositor_connection_id({});
    if (!compositor_connection_id.has_value() || !can_send_compositor_process_ipc(m_compositor_client))
        return false;
    m_compositor_client->async_connect_video_presentation(*compositor_connection_id, move(compositor_end));
    return true;
}

ErrorOr<IPC::TransportHandle> Application::connect_new_compositor_canvas_client()
{
    if (!m_compositor_client)
        return Error::from_string_literal("Compositor process is not available");

    // The Compositor treats every renderer-side connection uniformly; canvas-only
    // clients (workers) simply never register navigable contexts, so the
    // returned connection id is not needed.
    auto response_or_error = m_compositor_client->try_connect_web_content();
    if (response_or_error.is_error())
        return Error::from_string_literal("Compositor process disconnected while connecting a canvas client");
    return response_or_error.release_value().take_handle();
}

void Application::register_compositor_context(WebContentClient& web_content_client, Web::CompositorContextId context_id, Optional<Web::PageId> page_id)
{
    if (!can_send_compositor_process_ipc(m_compositor_client))
        return;

    if (auto result = try_register_compositor_context(web_content_client, context_id, page_id); result.is_error())
        dbgln("Unable to register Compositor context: {}", result.error());
}

ErrorOr<void> Application::try_register_compositor_context(WebContentClient& web_content_client, Web::CompositorContextId context_id, Optional<Web::PageId> page_id)
{
    if (!m_compositor_client)
        return Error::from_string_literal("Compositor process is not available");
    if (!web_content_client.is_open())
        return {};

    auto web_content_connection_id = web_content_client.compositor_connection_id({});
    if (!web_content_connection_id.has_value()) {
        TRY(connect_web_content_to_compositor(web_content_client));
        web_content_connection_id = web_content_client.compositor_connection_id({});
    }
    VERIFY(web_content_connection_id.has_value());

    auto compositor_page_id = page_id.map([](Web::PageId id) { return id.value(); });
    auto result = m_compositor_client->try_create_context(context_id, compositor_page_id, *web_content_connection_id);
    if (result.is_error())
        return Error::from_string_literal("Compositor process disconnected while creating context");

    // A page's context presents its frames to us, and takes its input from us, through our presentation connection.
    if (page_id.has_value() && m_compositor_presentation_client) {
        if (m_compositor_client->try_bind_presentation(context_id, m_compositor_presentation_id).is_error())
            return Error::from_string_literal("Compositor process disconnected while binding context");
    }

    return {};
}

void Application::update_compositor_viewport(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (!can_send_compositor_process_ipc(m_compositor_client))
        return;
    VERIFY(m_compositor_client);

    m_compositor_client->async_viewport_size_updated(context_id, viewport_size, device_pixel_ratio, window_resize_in_progress);
}

void Application::update_compositor_ui_overlays(Web::CompositorContextId context_id, Compositing::UIOverlays const& overlays)
{
    if (!can_send_compositor_process_ipc(m_compositor_client))
        return;
    VERIFY(m_compositor_client);

    m_compositor_client->async_set_ui_overlays(context_id, overlays);
}

void Application::update_compositor_display_metadata(Web::CompositorContextId context_id, Optional<u64> display_id, double refresh_rate)
{
    if (!can_send_compositor_process_ipc(m_compositor_client))
        return;
    VERIFY(m_compositor_client);

    m_compositor_client->async_set_display_metadata(context_id, display_id, sanitized_display_refresh_rate(refresh_rate));
}

void Application::update_compositor_context_visibility(Web::CompositorContextId context_id, Web::HTML::VisibilityState visibility_state)
{
    if (!can_send_compositor_process_ipc(m_compositor_client))
        return;
    VERIFY(m_compositor_client);

    auto context_visibility = visibility_state == Web::HTML::VisibilityState::Visible
        ? Compositing::ContextVisibility::Visible
        : Compositing::ContextVisibility::Hidden;
    m_compositor_client->async_set_context_visibility(context_id, context_visibility);
}

void Application::update_compositor_context_presents_from_cpu_memory(Web::CompositorContextId context_id, bool presents_from_cpu_memory)
{
    if (!can_send_compositor_process_ipc(m_compositor_client))
        return;
    VERIFY(m_compositor_client);

    m_compositor_client->async_set_context_presents_from_cpu_memory(context_id, presents_from_cpu_memory);
}

bool Application::handle_and_dispatch_key_event_in_compositor(Web::CompositorContextId context_id, Web::KeyEvent const& event, bool allow_keyboard_scroll)
{
    if (!can_send_compositor_process_ipc(m_compositor_presentation_client))
        return false;
    VERIFY(m_compositor_presentation_client);

    m_compositor_presentation_client->async_handle_and_dispatch_key_event(context_id, event.clone_without_browser_data(), allow_keyboard_scroll);
    return true;
}

bool Application::handle_and_dispatch_pinch_event_in_compositor(Web::CompositorContextId context_id, Web::PinchEvent const& event)
{
    if (!can_send_compositor_process_ipc(m_compositor_presentation_client))
        return false;
    VERIFY(m_compositor_presentation_client);

    m_compositor_presentation_client->async_handle_and_dispatch_pinch_event(context_id, event);
    return true;
}

bool Application::handle_and_dispatch_mouse_event_in_compositor(Web::CompositorContextId context_id, Web::MouseEvent const& event)
{
    if (!can_send_compositor_process_ipc(m_compositor_presentation_client))
        return false;
    VERIFY(m_compositor_presentation_client);

    m_compositor_presentation_client->async_handle_and_dispatch_mouse_event(context_id, event.clone_without_browser_data());
    return true;
}

void Application::notify_compositor_presented_bitmap_ready_to_paint(Web::CompositorContextId context_id, i32 bitmap_id)
{
    if (!can_send_compositor_process_ipc(m_compositor_presentation_client))
        return;
    VERIFY(m_compositor_presentation_client);

    m_compositor_presentation_client->async_presented_bitmap_ready_to_paint(context_id, bitmap_id);
}

void Application::crash_compositor_process()
{
    if (!can_send_compositor_process_ipc(m_compositor_client)) {
        warnln("Unable to crash Compositor process: process is not available");
        return;
    }
    VERIFY(m_compositor_client);

    m_compositor_client->async_crash();
}

bool Application::same_origin_tabs_may_share_a_process() const
{
    // NB: Tests run side by side, and must not take each other's state with them.
    return m_web_content_options.share_process_between_same_origin_tabs == ShareProcessBetweenSameOriginTabs::Yes
        && m_web_content_options.is_test_mode == IsTestMode::No;
}

ErrorOr<NonnullRefPtr<WebContentPage>> Application::open_page_for_new_tab(IsPrivate is_private)
{
    RefPtr<WebContentClient> web_content_client;
    if (is_private == IsPrivate::Yes) {
        web_content_client = TRY(create_web_content_client(IsPrivate::Yes, allocate_page_id()));
    } else if (m_spare_web_content_process) {
        web_content_client = m_spare_web_content_process.release_nonnull();
        launch_spare_web_content_process();
    } else {
        launch_spare_web_content_process();
        web_content_client = TRY(create_web_content_client(IsPrivate::No, allocate_page_id()));
    }
    return web_content_client->open_initial_page_for_new_top_level_traversable();
}

ErrorOr<NonnullRefPtr<WebContentClient>> Application::launch_web_content_process_without_page(IsPrivate is_private)
{
    return create_web_content_client(is_private, allocate_page_id());
}

ErrorOr<Application::ChildFrameWebContentProcess> Application::launch_child_frame_web_content_process(IsPrivate is_private, Vector<Web::HTML::RemoteNavigableDescriptor> remote_navigables, Web::HTML::CrossProcessId root_navigable_id, Web::HTML::SessionHistoryEntryDescriptor initial_history_entry, Web::HTML::VisibilityState system_visibility_state)
{
    auto page_id = allocate_page_id();
    auto initial_document_state_id = initial_history_entry.document_state.id;
    auto client = TRY(create_web_content_client(is_private, page_id, root_navigable_id, initial_document_state_id, move(remote_navigables), move(initial_history_entry), system_visibility_state));
    return ChildFrameWebContentProcess {
        .client = move(client),
        .page_id = page_id,
    };
}

void Application::launch_spare_web_content_process()
{
    // Spare WebContent processes inherit the active WebDriver endpoint, but they are not part of the
    // session and can race browser shutdown while bootstrapping.
    if (browser_options().webdriver_browser_endpoint.has_value())
        return;

    // Disable spare processes when debugging WebContent. Otherwise, it breaks running `gdb attach -p $(pidof WebContent)`.
    if (browser_options().debug_helper_processes.contains_slow(ProcessType::WebContent))
        return;
    // Disable spare processes when profiling WebContent. This reduces callgrind logging we are not interested in.
    if (browser_options().profile_helper_process == ProcessType::WebContent)
        return;

    if (m_spare_web_content_process || m_has_queued_task_to_launch_spare_web_content_process)
        return;
    m_has_queued_task_to_launch_spare_web_content_process = true;

    Core::deferred_invoke([this]() {
        m_has_queued_task_to_launch_spare_web_content_process = false;

        auto web_content_client = create_web_content_client(IsPrivate::No, allocate_page_id());
        if (web_content_client.is_error()) {
            dbgln("Unable to create spare web content client: {}", web_content_client.error());
            return;
        }

        m_spare_web_content_process = web_content_client.release_value();
        if (auto process = m_process_manager->find_child(m_spare_web_content_process->child_id()); process.has_value())
            process->set_title("(spare)"_utf16);
    });
}

// DevTools hears of every change to a jar, whoever made it. Each tab whose page keeps its storage in the changed
// bottle hears of it under that tab's own storage host: the host DevTools shows for a page and the key the browser
// keeps the page's storage under are two namings of one origin, and the tab's is the one its storage panel knows.
static void notify_devtools_of_storage_change(StorageJar& jar, StorageChange const& change)
{
    DevTools::DevToolsDelegate::StorageChange::Type type;
    switch (change.type) {
    case StorageChange::Type::Set:
        type = change.old_value.has_value() ? DevTools::DevToolsDelegate::StorageChange::Type::Changed : DevTools::DevToolsDelegate::StorageChange::Type::Added;
        break;
    case StorageChange::Type::Removed:
        type = DevTools::DevToolsDelegate::StorageChange::Type::Deleted;
        break;
    case StorageChange::Type::Cleared:
    case StorageChange::Type::Reset:
        type = DevTools::DevToolsDelegate::StorageChange::Type::Cleared;
        break;
    }

    Vector<DevTools::DevToolsDelegate::StorageChange> devtools_changes;
    devtools_changes.append({
        .storage_endpoint = change.storage_endpoint,
        .host = {},
        .type = type,
        .key = change.key.map([](auto const& key) { return key.to_utf8(); }),
    });

    // A reset says only that the bottle changed, not how; DevTools can't be told that, so it hears the bottle emptied
    // and then every item still in it added back.
    if (change.type == StorageChange::Type::Reset) {
        for (auto const& item : jar.get_all_items(change.storage_endpoint, change.storage_key)) {
            devtools_changes.append({
                .storage_endpoint = change.storage_endpoint,
                .host = {},
                .type = DevTools::DevToolsDelegate::StorageChange::Type::Added,
                .key = item.key.to_utf8(),
            });
        }
    }

    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        bool owns_jar = view.session().storage_jar.ptr() == &jar || (view.has_traversable() && &view.traversable().session_storage() == &jar);
        if (!owns_jar)
            return IterationDecision::Continue;
        auto host = DevTools::storage_host_for_url(view.url());
        if (!host.has_value() || DevTools::storage_key_for_host(*host) != change.storage_key)
            return IterationDecision::Continue;
        for (auto devtools_change : devtools_changes) {
            devtools_change.host = *host;
            view.notify_storage_changed(move(devtools_change));
        }
        return IterationDecision::Continue;
    });
}

ErrorOr<void> Application::launch_services()
{
    m_default_session = BrowsingSession::create(IsPrivate::No);
    StorageAreaRegistry::the().on_change_for_devtools = [this](StorageJar& jar, WebView::StorageChange const& change) {
        // Only an attached DevTools has anyone to tell, and the lookup below isn't free on a page's write path.
        if (m_devtools)
            notify_devtools_of_storage_change(jar, change);
    };

    m_default_session->permission_manager = PermissionManager::create_for_ordinary_session(LexicalPath::join(profile().paths().config, "Permissions.json"sv).string());
    m_permission_manager_observer = make<ApplicationPermissionManagerObserver>();
    m_default_session->permission_manager->register_observer(*m_permission_manager_observer);
    m_default_session->permission_manager->on_deadlines_changed = [this] { schedule_permission_expiry(); };
    update_permission_defaults();
    schedule_permission_expiry();

    m_settings_observer = make<ApplicationSettingsObserver>();

#if defined(AK_OS_WINDOWS)
    m_handle_broker_server = IPC::HandleBrokerServer::create();
#endif
    m_process_manager = make<ProcessManager>();
    m_process_manager->on_process_exited = [this](Process&& process, Optional<int> exit_status) {
        process_did_exit(move(process), exit_status);
    };

    if (m_browser_options.disable_sql_database == DisableSQLDatabase::No) {
        auto database_path = profile().paths().data;
        auto database = TRY(Database::Database::create(database_path, "Ladybird"sv));

        // The browsing database is shared by several stores, so the decision to fall back is
        // made for the file as a whole: if any store's schema is too new, none of them may
        // modify the file. The check-only preflight never writes, so the file is untouched
        // if any store then has to veto.
        auto cookies_outcome = TRY(CookieJar::migrate_schema(database, Database::MigrationMode::CheckOnly));
        auto hsts_outcome = TRY(HSTSStore::migrate_schema(database, Database::MigrationMode::CheckOnly));
        auto storage_outcome = TRY(StorageJar::migrate_schema(database, Database::MigrationMode::CheckOnly));
        auto downloads_outcome = TRY(DownloadStore::migrate_schema(database, Database::MigrationMode::CheckOnly));

        if (cookies_outcome == Database::MigrationOutcome::Success && hsts_outcome == Database::MigrationOutcome::Success && storage_outcome == Database::MigrationOutcome::Success && downloads_outcome == Database::MigrationOutcome::Success) {
            // Apply in order, stopping at the first store that finds the database too new
            // (a concurrent process may have migrated it since the preflight).
            cookies_outcome = TRY(CookieJar::migrate_schema(database));
            hsts_outcome = cookies_outcome == Database::MigrationOutcome::Success
                ? TRY(HSTSStore::migrate_schema(database))
                : Database::MigrationOutcome::DatabaseTooNew;
            storage_outcome = hsts_outcome == Database::MigrationOutcome::Success
                ? TRY(StorageJar::migrate_schema(database))
                : Database::MigrationOutcome::DatabaseTooNew;
            downloads_outcome = storage_outcome == Database::MigrationOutcome::Success
                ? TRY(DownloadStore::migrate_schema(database))
                : Database::MigrationOutcome::DatabaseTooNew;
        }

        // If any store finds the shared file too new, including a concurrent process migrating it
        // between the preflight and an apply above, none of them may persist this session, even if
        // an earlier store already migrated. Otherwise, we would keep writing to a vetoed database.
        if (cookies_outcome != Database::MigrationOutcome::Success || hsts_outcome != Database::MigrationOutcome::Success || storage_outcome != Database::MigrationOutcome::Success || downloads_outcome != Database::MigrationOutcome::Success) {
            warnln("Browsing database was created by a newer Ladybird version; cookies, web storage, HSTS policies and unfinished downloads will not be persisted this session");
            cookies_outcome = Database::MigrationOutcome::DatabaseTooNew;
            hsts_outcome = Database::MigrationOutcome::DatabaseTooNew;
            storage_outcome = Database::MigrationOutcome::DatabaseTooNew;
            downloads_outcome = Database::MigrationOutcome::DatabaseTooNew;
        }

        if (cookies_outcome == Database::MigrationOutcome::Success)
            m_default_session->cookie_jar = TRY(CookieJar::create(database));
        else
            m_default_session->cookie_jar = CookieJar::create();

        if (hsts_outcome == Database::MigrationOutcome::Success)
            m_default_session->hsts_store = TRY(HSTSStore::create(database));
        else
            m_default_session->hsts_store = HSTSStore::create();

        if (storage_outcome == Database::MigrationOutcome::Success)
            m_default_session->storage_jar = TRY(StorageJar::create(database));
        else
            m_default_session->storage_jar = StorageJar::create();

        if (downloads_outcome == Database::MigrationOutcome::Success)
            m_download_store = TRY(DownloadStore::create(database));
        else
            m_download_store = DownloadStore::create_disabled();

        // A headless run, a browser that WebDriver drives, and one that logs process statistics never join a browser
        // already running on their profile. Yet startup recovery would still rewrite the saved session, reset the
        // unclean-exit count, and demote the open windows of a browser running on this profile. So none of them keeps
        // its session, leaving the Sessions database as the last run left it.
        m_persists_sessions = !m_browser_options.headless_mode.has_value() && !m_browser_options.webdriver_browser_endpoint.has_value() && !m_stats_log;
    } else {
        m_default_session->cookie_jar = CookieJar::create();
        m_default_session->hsts_store = HSTSStore::create();
        m_default_session->storage_jar = StorageJar::create();
        m_download_store = DownloadStore::create_disabled();
    }

    m_default_session->blob_url_store = make<BlobURLStore>();

    m_file_downloader.adopt_download_store({}, *m_download_store);

#if !defined(AK_OS_WINDOWS)
    // A run stopped by SIGINT or SIGTERM ends like a quit, rather than like a crash: It saves the session, leaves its
    // windows for the next launch to recover, and records a clean exit.
    if (m_persists_sessions) {
        for (auto signal_number : { SIGINT, SIGTERM }) {
            m_session_signal_handlers.append(Core::EventLoop::register_signal(signal_number, [this](int) {
                quit_for_signal();
            }));
        }
    }
#endif

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

    m_image_decoder_manager = make<ImageDecoderManager>();
    TRY(launch_request_server());
    TRY(launch_compositor_process());

    if (m_browser_options.devtools_port.has_value()) {
        // Defer launching devtools until the entire application is initialized.
        Core::deferred_invoke([this]() {
            if (auto result = launch_devtools_server(); result.is_error())
                warnln("Unable to launch devtools server: {}", result.error());
        });
    }

    return {};
}

ErrorOr<void> Application::launch_compositor_process()
{
    VERIFY(!m_compositor_client);
    m_compositor_client = TRY(WebView::launch_compositor_process());
    m_compositor_client->on_death = [this]() {
        handle_compositor_process_death();
    };
    m_compositor_client->on_gpu_device_identity_reported = [this](Optional<Gfx::GpuDeviceIdentity> identity) {
        compositor_reported_gpu_device_identity(move(identity));
    };

    m_reported_compositor_gpu_presentation_unavailable = false;
    Optional<u64> gpu_presentation_adapter_luid;
#ifdef USE_DIRECTX
    gpu_presentation_adapter_luid = Gfx::default_dxgi_adapter_luid();
#endif
    auto presentation_or_error = m_compositor_client->try_create_presentation_connection(!platform_reports_scroll_momentum(), platform_scrolls_elastically(), gpu_presentation_adapter_luid);
    if (presentation_or_error.is_error())
        return Error::from_string_literal("Compositor process disconnected while creating presentation connection");
    auto presentation = presentation_or_error.release_value();
    auto transport = TRY(presentation.take_handle().create_transport());
    m_compositor_presentation_client = adopt_ref(*new CompositorPresentationClient(move(transport)));
    m_compositor_presentation_id = presentation.presentation_id();
    m_compositor_presentation_client->async_set_scrollbar_track_press_jumps(platform_scrollbar_track_press_jumps());
    m_compositor_presentation_client->on_death = [this, presentation_client = m_compositor_presentation_client->make_weak_ptr<CompositorPresentationClient>()] {
        // NB: This runs on a later turn, by which time a new Compositor can have replaced the one this belonged to.
        if (!presentation_client || presentation_client.ptr() != m_compositor_presentation_client.ptr())
            return;
        // NB: Our pages can neither show frames nor take input through the Compositor without this connection, so
        //     losing it is a Compositor failure. Closing the management connection ends the Compositor, and we
        //     restart it and rebind our pages as after a crash.
        if (m_compositor_client)
            m_compositor_client->shutdown();
    };

    return {};
}

void Application::compositor_reported_gpu_device_identity(Optional<Gfx::GpuDeviceIdentity> identity)
{
    auto previous_identity = exchange(m_compositor_gpu_device_identity, identity);

    // Frames decoded on another GPU cannot be displayed by this Compositor, so decoding starts over on its own. Each
    // MediaServer relaunches its server when it next asks for one.
    if (previous_identity.has_value() && *previous_identity != identity)
        shut_down_video_decoder_servers();

    auto callbacks = move(m_callbacks_awaiting_compositor_gpu_device_identity);
    for (auto& callback : callbacks)
        callback(identity);
}

void Application::when_compositor_gpu_device_identity_is_known(CompositorGpuDeviceIdentityCallback callback)
{
    if (m_compositor_gpu_device_identity.has_value()) {
        callback(*m_compositor_gpu_device_identity);
        return;
    }
    m_callbacks_awaiting_compositor_gpu_device_identity.append(move(callback));
}

void Application::platform_scrollbar_track_press_behavior_changed()
{
    if (m_compositor_presentation_client)
        m_compositor_presentation_client->async_set_scrollbar_track_press_jumps(platform_scrollbar_track_press_jumps());
}

void Application::notify_compositor_gpu_presentation_unavailable()
{
    if (m_reported_compositor_gpu_presentation_unavailable)
        return;
    m_reported_compositor_gpu_presentation_unavailable = true;
    if (m_compositor_presentation_client)
        m_compositor_presentation_client->async_set_client_gpu_presentation_capability(false, 0);
}

void Application::handle_compositor_process_death()
{
    m_compositor_client = nullptr;
    if (m_compositor_presentation_client)
        m_compositor_presentation_client->on_death = nullptr;
    m_compositor_presentation_client = nullptr;

    // Nothing will forward or hand back the input events the dead compositor still held.
    WebContentClient::for_each_client([](WebContentClient& client) {
        client.for_each_page([](WebContentPage& page) {
            if (page.displays_tab())
                page.view().discard_input_events_routed_through_lost_compositor({});
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });

    if (Core::EventLoop::current().was_exit_requested())
        return;
    switch (m_compositor_recovery_state) {
    case CompositorRecoveryState::Idle:
        break;
    case CompositorRecoveryState::Queued:
        return;
    case CompositorRecoveryState::Recovering:
        warnln("Compositor process died while recovering, crashing Browser");
        VERIFY_NOT_REACHED();
    }

    m_compositor_recovery_state = CompositorRecoveryState::Queued;
    Core::deferred_invoke([this] {
        VERIFY(m_compositor_recovery_state == CompositorRecoveryState::Queued);
        m_compositor_recovery_state = CompositorRecoveryState::Idle;
        recover_compositor_process();
    });

    if (on_compositor_process_death)
        on_compositor_process_death();
}

void Application::recover_compositor_process()
{
    if (Core::EventLoop::current().was_exit_requested())
        return;

    constexpr size_t max_compositor_restart_count = 3;
    if (m_compositor_restart_count >= max_compositor_restart_count) {
        warnln("Compositor process crashed repeatedly, crashing Browser");
        VERIFY_NOT_REACHED();
    }

    ++m_compositor_restart_count;
    dbgln("Compositor process died, restarting ({}/{})", m_compositor_restart_count, max_compositor_restart_count);

    VERIFY(m_compositor_recovery_state == CompositorRecoveryState::Idle);
    m_compositor_recovery_state = CompositorRecoveryState::Recovering;
    ScopeGuard clear_recovery_flag = [this] {
        m_compositor_recovery_state = CompositorRecoveryState::Idle;
    };

    if (auto result = launch_compositor_process(); result.is_error()) {
        warnln("Unable to restart Compositor process: {}", result.error());
        VERIFY_NOT_REACHED();
    }

    Vector<NonnullRefPtr<WebContentClient>> clients;
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.is_open())
            clients.append(NonnullRefPtr { client });
        return IterationDecision::Continue;
    });

    for (auto& client : clients) {
        if (auto result = client->reconnect_to_compositor_process({}); result.is_error()) {
            warnln("Unable to reconnect WebContent process {} to Compositor: {}", client->pid(), result.error());
            VERIFY_NOT_REACHED();
        }
    }
    for (auto& client : clients) {
        if (auto result = client->recreate_compositor_contexts({}); result.is_error()) {
            warnln("Unable to recreate Compositor contexts for WebContent process {}: {}", client->pid(), result.error());
            VERIFY_NOT_REACHED();
        }
    }
    for (auto& client : clients)
        client->replay_compositor_view_state_after_reconnect({});
    for (auto& client : clients)
        client->notify_compositor_process_reconnected({});
}

ErrorOr<void> Application::launch_request_server()
{
    m_request_server_manager = make<RequestServerManager>();
    (void)TRY(m_request_server_manager->browser_instance(*m_default_session));

    if (m_browser_options.dns_settings.has_value())
        m_settings->set_dns_settings(m_browser_options.dns_settings.value(), true);

    return {};
}

void Application::configure_request_server_control_client(Requests::RequestControlClient& control_client)
{
    control_client.on_store_response_cookies_and_hsts_policy = [](int client_id, URL::URL const& url, Optional<HTTP::Cookie::PartitionContext> const& partition_context, HTTP::Cookie::SameSiteContext same_site_context, Vector<HTTP::Cookie::ParsedCookie> const& cookies, Optional<HTTP::HSTS::ParsedHSTSPolicy> const& hsts_policy) {
        auto session = the().session_for_request_server_client(client_id);
        if (!session)
            return;

        if (the().request_server_client_may_use_cookies_in(client_id, url, partition_context)) {
            for (auto const& cookie : cookies)
                session->cookie_jar->set_cookie(url, cookie, HTTP::Cookie::Source::Http, partition_context, same_site_context);
        }

        if (hsts_policy.has_value() && url.host().has_value() && url.host()->is_domain())
            session->hsts_store->store_policy(url.host()->get<String>(), *hsts_policy);
    };

    control_client.on_retrieve_http_cookie = [](int client_id, URL::URL const& url, Optional<HTTP::Cookie::PartitionContext> const& partition_context, HTTP::Cookie::SameSiteContext same_site_context) -> String {
        auto session = the().session_for_request_server_client(client_id);
        if (!session)
            return {};
        if (!the().request_server_client_may_use_cookies_in(client_id, url, partition_context))
            return {};
        auto& cookie_jar = *session->cookie_jar;
        if constexpr (!REQUESTSERVER_WIRE_DEBUG)
            return cookie_jar.get_cookie(url, HTTP::Cookie::Source::Http, partition_context, same_site_context);
        auto started_at = MonotonicTime::now();
        auto cookie = cookie_jar.get_cookie(url, HTTP::Cookie::Source::Http, partition_context, same_site_context);
        auto elapsed_ms = (MonotonicTime::now() - started_at).to_milliseconds();
        if (elapsed_ms > 5) {
            dbgln("UI wire-cookie: get_cookie({}) took {} ms ({} bytes returned)",
                url, elapsed_ms, cookie.bytes().size());
        }
        return cookie;
    };

    TabPerformanceMonitor::configure(control_client);
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

ErrorOr<void> Application::launch_devtools_client()
{
    if (!m_devtools || !m_browser_options.devtools_port.has_value())
        return Error::from_string_literal("DevTools is not currently enabled");

    if (m_devtools->has_active_connection())
        return Error::from_string_literal("A DevTools client is already connected");

    auto view = active_web_view();
    if (!view.has_value())
        return Error::from_string_literal("No active tab is available for DevTools");

    if (!m_devtools_client)
        m_devtools_client = DevTools::FirefoxClient::create();

    return m_devtools_client->ensure_running(*m_browser_options.devtools_port, view->view_id());
}

static NonnullRefPtr<Core::Timer> load_page_for_screenshot_and_exit(Core::EventLoop& event_loop, HeadlessWebView& view, URL::URL const& url, u32 screenshot_timeout)
{
    outln("Taking screenshot after {} seconds", screenshot_timeout);

    auto timer = Core::Timer::create_single_shot(
        AK::Duration::from_seconds(screenshot_timeout),
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

        view.request_internal_page_info(type)
            ->when_resolved([&event_loop](auto const& text) {
                outln("{}", text);
                event_loop.quit(0);
            })
            .when_rejected([&event_loop](auto const& error) {
                warnln("Unable to get page info: {}", error);
                event_loop.quit(1);
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

#if !defined(AK_OS_WINDOWS)
        if (*m_browser_options.headless_mode == HeadlessMode::Terminal)
            view = TRY(TerminalWebView::create(move(theme), m_browser_options.device_pixel_ratio));
        else
#endif
            view = HeadlessWebView::create(move(theme), { m_browser_options.window_width, m_browser_options.window_height });

        if (!m_browser_options.webdriver_browser_endpoint.has_value()) {
            if (m_browser_options.urls.size() != 1)
                return Error::from_string_literal("Headless mode currently only supports exactly one URL");

            switch (*m_browser_options.headless_mode) {
            case HeadlessMode::Screenshot:
                screenshot_timer = load_page_for_screenshot_and_exit(*m_event_loop, *view, m_browser_options.urls.first(), m_browser_options.screenshot_delay);
                break;
            case HeadlessMode::LayoutTree:
                load_page_for_info_and_exit(*m_event_loop, *view, m_browser_options.urls.first(), WebView::PageInfoType::LayoutTree);
                break;
            case HeadlessMode::Text:
                load_page_for_info_and_exit(*m_event_loop, *view, m_browser_options.urls.first(), WebView::PageInfoType::Text);
                break;
            case HeadlessMode::Manual:
            case HeadlessMode::Terminal:
                load_page_and_exit_on_close(*m_event_loop, *view, m_browser_options.urls.first());
                break;
            case HeadlessMode::Test:
                VERIFY_NOT_REACHED();
            }
        }
    }

    return m_event_loop->exec();
}

Core::EventLoop& Application::create_platform_event_loop()
{
    return Core::EventLoop::initialize_for_current_thread();
}

NonnullOwnPtr<MicrophoneAuthorizer> Application::create_microphone_authorizer()
{
    return make<NoOpMicrophoneAuthorizer>();
}

ChildIdentity Application::add_child_process(WebView::Process&& process)
{
    return m_process_manager->add_process(move(process));
}

#if defined(AK_OS_MACH)
void Application::set_process_task_name_port(pid_t pid, Core::MachPort&& port)
{
    m_process_manager->set_process_task_name_port(pid, move(port));
}
#endif

#if defined(AK_OS_MACOS)
void Application::set_browser_process_transport_handler(Function<void(NonnullOwnPtr<IPC::Transport>)> handler)
{
    m_on_browser_process_transport = move(handler);
}
#endif

Optional<Process&> Application::find_process(pid_t pid)
{
    return m_process_manager->find_process(pid);
}

void Application::process_did_exit(Process&& process, Optional<int>)
{
#if defined(AK_OS_MACOS)
    // A helper that exits before it fetches its transport leaves its ports registered. Holding them would keep the
    // other end of the transport from noticing that the helper is gone.
    m_transport_bootstrap_server.unregister_child_transport(process.pid());
#endif

    if (m_event_loop->was_exit_requested())
        return;

    m_image_decoder_manager->process_did_exit(process.child_id());

    dbgln_if(WEBVIEW_PROCESS_DEBUG, "Process {} died, type: {}", process.pid(), process_name_from_type(process.type()));

    auto const& report_name = process.saved_crash_report_name();

    switch (process.type()) {
    case ProcessType::Compositor:
        if (auto client = process.client<CompositorClient>()) {
            if (auto on_death = move(client->on_death))
                on_death();
        }
        break;
    case ProcessType::ImageDecoder:
        // The processes it served ask for a new one when they next have an image to decode.
        break;
    case ProcessType::MediaServer:
        // The connection's holder launches a new MediaServer when it next connects a client.
        break;
    case ProcessType::AudioServer:
        // The connection's holder launches a new AudioServer when it next connects a client; the streams of the old
        // one report their loss and reopen.
        break;
    case ProcessType::VideoDecoderServer:
        // The MediaServer it served asks for a new one once its decoders notice the loss.
        break;
    case ProcessType::WebRTCClient:
        // The WebContent process it served asks for a new one the next time it needs one.
        break;
    case ProcessType::RequestServer:
        if (auto client = process.client<Requests::RequestControlClient>()) {
            dbgln_if(WEBVIEW_PROCESS_DEBUG, "Restart request server");
            if (auto on_request_server_died = move(client->on_request_server_died))
                on_request_server_died();
        }
        break;
    case ProcessType::WebContent:
        if (auto client = process.client<WebContentClient>()) {
            client->did_lose_process();
            if (!report_name.is_empty())
                client->did_save_crash_report(report_name);
            m_web_content_clients.remove(client.release_nonnull());
        }
        break;
    case ProcessType::Browser:
    case ProcessType::UI:
    case ProcessType::Hub:
        dbgln("Invalid process type to be dying: {}", process_name_from_type(process.type()));
        VERIFY_NOT_REACHED();
    }

    if (report_name.is_empty())
        return;

    // A report waits until it is answered, on a tab's crash screen or in the list of reports.
    CrashReportOutbox::the().reports_changed();
}

static bool download_path_is_available(LexicalPath const& path)
{
    if (FileSystem::exists(path.string()))
        return false;

    for (auto const& download : Application::the().file_downloader().downloads()) {
        if (FileDownloader::status_is_active(download.status) && download.destination.string() == path.string())
            return false;
    }

    return true;
}

static LexicalPath unique_download_path(ByteString const& downloads_directory, ByteString const& file)
{
    auto destination = LexicalPath::join(downloads_directory, file.view());
    if (download_path_is_available(destination))
        return destination;

    for (u64 index = 1;; ++index) {
        auto candidate_filename = Web::numbered_download_filename(file, index);
        auto candidate = LexicalPath::join(downloads_directory, candidate_filename.view());
        if (download_path_is_available(candidate))
            return candidate;
    }
}

ErrorOr<LexicalPath> Application::default_path_for_downloaded_file(ByteString const& file) const
{
    auto downloads_directory = Core::StandardPaths::downloads_directory();

    if (!FileSystem::is_directory(downloads_directory)) {
        dbgln("Unable to find download folder, please ensure {} is a directory or use the XDG_DOWNLOAD_DIR environment variable to set a new download directory", downloads_directory);
        return Error::from_errno(ENOENT);
    }

    return unique_download_path(downloads_directory, Web::sanitize_suggested_download_filename(file));
}

NonnullRefPtr<Core::Promise<LexicalPath>> Application::path_for_downloaded_file(ByteString const& file) const
{
    if (browser_options().headless_mode.has_value()) {
        auto download_path = default_path_for_downloaded_file(file);
        if (download_path.is_error())
            return Core::Promise<LexicalPath>::rejected(download_path.release_error());
        return Core::Promise<LexicalPath>::resolved(download_path.release_value());
    }

    return ask_user_for_download_path(Web::sanitize_suggested_download_filename(file))->map<LexicalPath>([](auto& download_path) {
        return LexicalPath { download_path };
    });
}

NonnullRefPtr<Core::Promise<ByteString>> Application::ask_user_for_download_path(ByteString const&) const
{
    return Core::Promise<ByteString>::rejected(Error::from_errno(ECANCELED));
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    outln("{} saved to: {}", download_name, path);
}

void Application::display_error_dialog(StringView error_message) const
{
    warnln("{}", error_message);
}

bool Application::supports_clipboard_type(ClipboardType type) const
{
    return type == ClipboardType::Text;
}

bool Application::supports_picture_in_picture() const
{
    return browser_options().headless_mode.has_value();
}

NonnullOwnPtr<NotificationBackend> Application::create_notification_backend()
{
    return make<NoOpNotificationBackend>();
}

OwnPtr<PictureInPictureWindow> Application::create_picture_in_picture_window(WebContentPage& requesting_page, CanonicalTraversable& traversable, Gfx::IntSize video_size)
{
    if (!browser_options().headless_mode.has_value())
        return {};

    // Every view in a headless browser is a HeadlessWebView.
    auto& requesting_view = static_cast<HeadlessWebView&>(requesting_page.view());
    return HeadlessWebView::create_picture_in_picture_window(requesting_view, traversable, video_size);
}

Utf16String Application::clipboard_text(ClipboardType) const
{
    for (auto const& representation : m_clipboard.system_clipboard_representations) {
        if (representation.name == "text/plain"sv) {
            if (auto const* data = representation.data.get_pointer<ByteString>())
                return Utf16String::from_utf8(*data);
        }
    }
    return {};
}

void Application::set_clipboard_text(String text, ClipboardType)
{
    m_clipboard = Web::Clipboard::SystemClipboardItem {
        .system_clipboard_representations = {
            {
                .name = "text/plain"_string,
                .data = text.to_byte_string(),
            },
        },
    };
}

void Application::insert_clipboard_entry(Web::Clipboard::SystemClipboardRepresentation entry)
{
    insert_clipboard_item({ { move(entry) } });
}

NonnullRefPtr<Core::Promise<Application::BrowsingDataSizes>> Application::estimate_browsing_data_size_accessed_since(UnixDateTime since)
{
    auto promise = Core::Promise<BrowsingDataSizes>::construct();

    m_request_server_manager->estimate_cache_size_accessed_since(since)
        ->when_resolved([this, promise, since](Requests::CacheSizes cache_sizes) {
            auto cookie_sizes = m_default_session->cookie_jar->estimate_storage_size_accessed_since(since);
            auto storage_sizes = m_default_session->storage_jar->estimate_storage_size_accessed_since(since);

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

NonnullRefPtr<Core::Promise<Empty>> Application::clear_browsing_data(ClearBrowsingDataOptions const& options)
{
    RefPtr<Core::Promise<Empty>> promise;
    bool did_change_history = false;

    if (options.delete_cached_files == ClearBrowsingDataOptions::Delete::Yes) {
        promise = m_request_server_manager->clear_cache(options.since);

        // FIXME: Maybe we should forward the "since" parameter to the WebContent process, but the in-memory cache is
        //        transient anyways, so just assuming they were all accessed in the last hour is fine for now.
        ViewImplementation::for_each_view([](ViewImplementation& view) {
            // FIXME: This should be promoted from a debug request to a proper endpoint.
            view.debug_request("clear-cache"sv);
            return IterationDecision::Continue;
        });
    }

    if (options.delete_history == ClearBrowsingDataOptions::Delete::Yes) {
        if (auto result = CrashReportStore::the().remove_websites_saved_since(options.since); result.is_error())
            dbgln("Unable to remove the websites of crash reports: {}", result.error());
        CrashReportOutbox::the().reports_changed();
        did_change_history = true;
    }

    if (options.delete_download_history == ClearBrowsingDataOptions::Delete::Yes)
        m_file_downloader.remove_inactive_downloads_created_since(options.since);

    if (options.delete_site_data == ClearBrowsingDataOptions::Delete::Yes) {
        m_default_session->cookie_jar->expire_cookies_accessed_since(options.since);
        m_default_session->storage_jar->remove_items_accessed_since(options.since);
        m_default_session->hsts_store->remove_policies_observed_since(options.since);

        if (auto private_session = m_private_session.strong_ref()) {
            private_session->cookie_jar->expire_cookies_accessed_since(options.since);
            private_session->storage_jar->remove_items_accessed_since(options.since);
            private_session->hsts_store->remove_policies_observed_since(options.since);
        }
    }

    if (did_change_history)
        did_clear_history_since(options.since);

    if (!promise) {
        promise = Core::Promise<Empty>::construct();

        Core::deferred_invoke([promise]() {
            promise->resolve({});
        });
    }

    return promise.release_nonnull();
}

void Application::initialize_page_options()
{
    m_user_agent_string = MUST(String::from_utf8(m_web_content_options.user_agent_preset.has_value()
            ? *WebView::user_agents.get(*m_web_content_options.user_agent_preset)
            : Web::default_user_agent));
    m_scripting_enabled = m_browser_options.disable_scripting == DisableScripting::No;
    m_blocks_pop_ups = m_browser_options.allow_popups == AllowPopups::No;
    update_website_color_scheme();

    create_platform_actions();
}

void Application::set_preferred_contrast(Web::CSS::PreferredContrast contrast)
{
    m_contrast = contrast;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        view.set_preferred_contrast(m_contrast);
        return IterationDecision::Continue;
    });
}

void Application::set_preferred_motion(Web::CSS::PreferredMotion motion)
{
    m_motion = motion;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        view.set_preferred_motion(m_motion);
        return IterationDecision::Continue;
    });
}

void Application::set_user_agent_string(String user_agent)
{
    m_user_agent_string = move(user_agent);

    // Workers take no debug requests; the worker process manager hands the new value to every live worker.
    WorkerAgentManager::the().update_user_agent(m_user_agent_string);

    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        view.debug_request("spoof-user-agent"sv, m_user_agent_string.to_byte_string());
        view.debug_request("clear-cache"sv); // Clear the cache to ensure requests are re-done with the new value.
        return IterationDecision::Continue;
    });
}

void Application::set_navigator_compatibility_mode(Web::NavigatorCompatibilityMode mode)
{
    m_navigator_compatibility_mode = mode;

    // Workers take no debug requests; the worker process manager hands the new value to every live worker.
    WorkerAgentManager::the().update_navigator_compatibility_mode(m_navigator_compatibility_mode);

    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        view.debug_request("navigator-compatibility-mode"sv, navigator_compatibility_mode_to_string(m_navigator_compatibility_mode));
        view.debug_request("clear-cache"sv); // Clear the cache to ensure requests are re-done with the new value.
        return IterationDecision::Continue;
    });
}

static void send_debug_switch_to_every_view(StringView request, bool on)
{
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        view.debug_request(request, on ? "on"sv : "off"sv);
        return IterationDecision::Continue;
    });
}

void Application::set_scripting_enabled(bool enabled)
{
    m_scripting_enabled = enabled;
    send_debug_switch_to_every_view("scripting"sv, enabled);
}

void Application::set_blocks_pop_ups(bool blocks)
{
    m_blocks_pop_ups = blocks;
    send_debug_switch_to_every_view("block-pop-ups"sv, blocks);
}

void Application::set_shows_line_box_borders(bool shows)
{
    m_shows_line_box_borders = shows;
    send_debug_switch_to_every_view("set-line-box-borders"sv, shows);
}

void Application::set_shows_caret_hit_test_debug_overlay(bool shows)
{
    m_shows_caret_hit_test_debug_overlay = shows;
    send_debug_switch_to_every_view("set-caret-hit-test-debug-overlay"sv, shows);
}

void Application::dump_cookies() const
{
    m_default_session->cookie_jar->dump_cookies();
}

// The options every page hosting the tab's document runs with.
void Application::apply_view_options(Badge<ViewImplementation>, ViewImplementation& view, WebContentPage& page)
{
    view.set_preferred_color_scheme(m_color_scheme);
    view.set_preferred_contrast(m_contrast);
    view.set_preferred_motion(m_motion);

    apply_content_blocking_policy(page);

    page.async_debug_request("set-line-box-borders"sv, m_shows_line_box_borders ? "on"sv : "off"sv);
    page.async_debug_request("set-caret-hit-test-debug-overlay"sv, m_shows_caret_hit_test_debug_overlay ? "on"sv : "off"sv);
    page.async_debug_request("scripting"sv, m_scripting_enabled ? "on"sv : "off"sv);
    page.async_debug_request("block-pop-ups"sv, m_blocks_pop_ups ? "on"sv : "off"sv);
    page.async_debug_request("spoof-user-agent"sv, m_user_agent_string);
    page.async_debug_request("navigator-compatibility-mode"sv, navigator_compatibility_mode_to_string(m_navigator_compatibility_mode));
}

bool Application::sites_can_ask_for_location() const
{
    // NB: Tests and WebDriver sessions get their positions from emulation, so they need no provider.
    auto has_location_source = Core::GeolocationProvider::is_available() || web_content_options().is_test_mode == IsTestMode::Yes
        || browser_options().webdriver_browser_endpoint.has_value();
    return settings().permission_policy(Web::Permissions::PermissionName::Geolocation) == PermissionPolicy::Ask && has_location_source;
}

void Application::permissions_changed(Badge<ApplicationPermissionManagerObserver>, PermissionManager const& manager)
{
    // A revoked site loses what it was using before its pages hear of the change.
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.session().permission_manager.ptr() == &manager)
            view.revoke_geolocation_without_permission();
        return IterationDecision::Continue;
    });
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.session().permission_manager.ptr() == &manager)
            client.update_audio_capture_permission();
        return IterationDecision::Continue;
    });

    // Each process hears only about the sites it asked about, which are sites it shows.
    WebContentClient::for_each_client([&](WebContentClient& client) {
        if (client.session().permission_manager.ptr() == &manager)
            client.async_set_permission_snapshot(manager.snapshot_for(client.permission_origins()));
        return IterationDecision::Continue;
    });
    WorkerAgentManager::the().update_permission_snapshot(manager);

    schedule_permission_expiry();
}

void Application::schedule_permission_expiry()
{
    Optional<UnixDateTime> deadline;
    auto consider = [&](PermissionManager const& manager) {
        if (auto next = manager.next_deadline(); next.has_value() && (!deadline.has_value() || *next < *deadline))
            deadline = next;
    };
    consider(*m_default_session->permission_manager);
    if (auto private_session = m_private_session.strong_ref())
        consider(*private_session->permission_manager);

    if (!deadline.has_value()) {
        if (m_permission_expiry_timer)
            m_permission_expiry_timer->stop();
        return;
    }

    auto delay = max(*deadline - UnixDateTime::now(), AK::Duration::zero());
    if (!m_permission_expiry_timer) {
        m_permission_expiry_timer = Core::Timer::create_single_shot(AK::Duration::zero(), [this] {
            m_default_session->permission_manager->run_expiry();
            if (auto private_session = m_private_session.strong_ref())
                private_session->permission_manager->run_expiry();
            schedule_permission_expiry();
        });
    }
    // NB: The timer's resolution is a millisecond, so wait one more to land at or after the deadline.
    m_permission_expiry_timer->restart(delay + AK::Duration::from_milliseconds(1));
}

void Application::permission_settings_changed(Badge<ApplicationSettingsObserver>)
{
    update_permission_defaults();
}

void Application::update_os_location_authorization()
{
    auto denied = m_geolocation_provider && m_geolocation_provider->authorization() == Core::GeolocationAuthorization::Denied;
    m_default_session->permission_manager->set_os_denies_geolocation(denied);
}

Core::GeolocationAuthorization Application::geolocation_authorization()
{
    if (!sites_can_ask_for_location())
        return Core::GeolocationAuthorization::Unknown;
    auto provider = ensure_geolocation_provider();
    if (provider.is_error())
        return Core::GeolocationAuthorization::Unknown;
    return provider.value()->authorization();
}

void Application::update_permission_defaults()
{
    // A private session reads its defaults from the ordinary one.
    for (auto name : Web::Permissions::PERMISSION_NAMES) {
        auto state = [&]() {
            switch (settings().permission_policy(name)) {
            case PermissionPolicy::Ask:
                return name == Web::Permissions::PermissionName::Geolocation && !sites_can_ask_for_location()
                    ? Web::Permissions::EffectiveState::Denied
                    : Web::Permissions::EffectiveState::Prompt;
            case PermissionPolicy::Block:
                return Web::Permissions::EffectiveState::Denied;
            }
            VERIFY_NOT_REACHED();
        }();

        m_default_session->permission_manager->set_global_default(name, state);
    }
}

void Application::languages_changed(Badge<ApplicationSettingsObserver>)
{
    WorkerAgentManager::the().update_preferred_languages(m_settings->languages());
}

String Application::user_agent_string() const
{
    return m_user_agent_string;
}

Web::NavigatorCompatibilityMode Application::navigator_compatibility_mode() const
{
    return m_navigator_compatibility_mode;
}

static Web::CSS::PreferredColorScheme preferred_color_scheme_for(ThemePreference preference)
{
    switch (preference) {
    case ThemePreference::System:
        return Web::CSS::PreferredColorScheme::Auto;
    case ThemePreference::Light:
        return Web::CSS::PreferredColorScheme::Light;
    case ThemePreference::Dark:
        return Web::CSS::PreferredColorScheme::Dark;
    }
    VERIFY_NOT_REACHED();
}

Web::CSS::PreferredColorScheme Application::interface_color_scheme() const
{
    return preferred_color_scheme_for(m_settings->appearance().interface_theme);
}

void Application::update_website_color_scheme()
{
    auto color_scheme = preferred_color_scheme_for(m_settings->appearance().website_color_scheme);
    if (color_scheme == m_color_scheme)
        return;
    m_color_scheme = color_scheme;

    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        view.set_preferred_color_scheme(m_color_scheme);
        return IterationDecision::Continue;
    });
}

void Application::appearance_changed(Badge<ApplicationSettingsObserver>)
{
    update_website_color_scheme();
}

void Application::tab_settings_changed(Badge<ApplicationSettingsObserver>)
{
    update_tabs_display();
}

void Application::background_networking_settings_changed(Badge<ApplicationSettingsObserver>)
{
    if (!m_settings->automatic_filter_list_updates_allowed()) {
        m_pending_content_blocker_list_updates.remove_all_matching([](auto const& update) { return update.trigger == ContentBlockerListUpdateTrigger::Automatic; });
        if (m_active_content_blocker_list_update.has_value() && m_active_content_blocker_list_update->trigger == ContentBlockerListUpdateTrigger::Automatic)
            stop_current_content_blocker_list_update_and_continue();
        return;
    }

    start_content_blocker_list_update(ContentBlockerListUpdateTrigger::Automatic);
}

bool Application::content_blocking_enabled() const
{
    return m_browser_options.enable_content_blocker == EnableContentBlocker::Yes && m_settings->content_blocker_enabled();
}

bool Application::has_content_blocking_rules() const
{
    return m_content_blocker_list_buffer.has_value() && m_content_blocker_list_buffer->size() > 0;
}

void Application::apply_content_blocking_policy(WebContentPage& page)
{
    auto disabled_sites = m_settings->content_blocker_disabled_sites().values();
    page.async_set_content_blocking_policy(content_blocking_enabled(), disabled_sites);
}

void Application::content_blocker_settings_changed(Badge<ApplicationSettingsObserver>)
{
    apply_content_blocker_settings();
}

void Application::apply_content_blocker_settings()
{
    rebuild_content_blocker_list_paths();
    bool rules_changed = false;

    if (auto result = load_content_blocker_lists(); result.is_error())
        warnln("Unable to apply content blocker settings: {}", result.error());
    else
        rules_changed = result.release_value();

    auto enabled = content_blocking_enabled();
    auto disabled_sites = m_settings->content_blocker_disabled_sites().values();

    WebContentClient::for_each_client([&](WebContentClient& client) {
        client.for_each_page([&](WebContentPage& page) {
            client.async_set_content_blocking_policy(page.id(), enabled, disabled_sites);
            return IterationDecision::Continue;
        });
        if (rules_changed)
            client.async_set_content_blockers(*m_content_blocker_list_buffer);
        return IterationDecision::Continue;
    });
    ViewImplementation::for_each_view([](ViewImplementation& view) {
        view.notify_content_blocking_changed();
        return IterationDecision::Continue;
    });
}

bool Application::content_blocker_list_update_in_progress() const
{
    return m_active_content_blocker_list_update.has_value() || !m_pending_content_blocker_list_updates.is_empty();
}

void Application::update_content_blocker_lists(Badge<SettingsUI>)
{
    start_content_blocker_list_update(ContentBlockerListUpdateTrigger::UserInitiated);
}

void Application::download_content_blocker_list_if_needed(Badge<Application, SettingsUI>, StringView identifier)
{
    if (content_blocker_list_last_updated_at(identifier).has_value())
        return;
    start_content_blocker_list_update(ContentBlockerListUpdateTrigger::UserInitiated, identifier);
}

// The shared actions that depend on the page follow the active view, whichever tab or window made it the active one.
void Application::set_spell_checker(OwnPtr<SpellChecker> spell_checker)
{
    m_spell_checker = move(spell_checker);
}

void Application::set_translator(OwnPtr<Translator> translator)
{
    m_translator = move(translator);
    if (!m_translator)
        return;
    m_translator->on_languages_changed = [] {
        ViewImplementation::for_each_view([](ViewImplementation& view) {
            view.page_translation().did_change_installed_languages();
            return IterationDecision::Continue;
        });
    };
}

MicrophoneAuthorizer& Application::microphone_authorizer()
{
    if (!m_microphone_authorizer)
        m_microphone_authorizer = create_microphone_authorizer();
    return *m_microphone_authorizer;
}

ErrorOr<NonnullRawPtr<Core::GeolocationProvider>> Application::ensure_geolocation_provider()
{
    if (!m_geolocation_provider) {
        if (!should_use_os_geolocation())
            return Error::from_string_literal("The OS location provider is disabled");
        m_geolocation_provider = TRY(Core::GeolocationProvider::create());
        m_geolocation_provider->on_authorization_change = [this] { update_os_location_authorization(); };
        update_os_location_authorization();
    }

    return NonnullRawPtr { *m_geolocation_provider };
}

static Core::GeolocationError geolocation_provider_unavailable_error(Error const& error)
{
    return { Core::GeolocationError::Type::PositionUnavailable, MUST(String::from_utf8(error.string_literal())) };
}

ErrorOr<Core::GeolocationProvider::RequestId, Core::GeolocationError> Application::request_geolocation_position(Core::GeolocationProvider::SuccessCallback on_success, Core::GeolocationProvider::ErrorCallback on_error)
{
    auto provider = ensure_geolocation_provider();
    if (provider.is_error())
        return geolocation_provider_unavailable_error(provider.error());

    return provider.value()->request_current_position(move(on_success), move(on_error));
}

void Application::cancel_geolocation_position_request(Core::GeolocationProvider::RequestId request_id)
{
    if (!m_geolocation_provider)
        return;

    m_geolocation_provider->cancel_current_position_request(request_id);
}

ErrorOr<Core::GeolocationProvider::WatchId, Core::GeolocationError> Application::start_watching_geolocation_position(Core::GeolocationProvider::SuccessCallback on_success, Core::GeolocationProvider::ErrorCallback on_error)
{
    auto provider = ensure_geolocation_provider();
    if (provider.is_error())
        return geolocation_provider_unavailable_error(provider.error());

    return provider.value()->start_watching_position(move(on_success), move(on_error));
}

void Application::stop_watching_geolocation_position(Core::GeolocationProvider::WatchId watch_id)
{
    if (!m_geolocation_provider)
        return;

    m_geolocation_provider->stop_watching_position(watch_id);
}

ErrorOr<void> Application::toggle_devtools_enabled()
{
    if (m_devtools) {
        m_devtools_client.clear();
        m_devtools.clear();
        on_devtools_disabled();
    } else {
        TRY(launch_devtools_server());
    }

    return {};
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
        if (!view.owner_view_id().has_value())
            tabs.empend(view.view_id(), view.title().to_utf8(), view.url().to_string());
        return IterationDecision::Continue;
    });

    return tabs;
}

Vector<DevTools::CSSProperty> Application::css_property_list() const
{
    Vector<DevTools::CSSProperty> property_list;

    for (auto const& entry : Web::CSS::property_list()) {
        DevTools::CSSProperty property;
        property.name = MUST(String::from_utf8(entry.name));
        property.is_inherited = entry.is_inherited;
        property_list.append(move(property));
    }

    return property_list;
}

void Application::reload_tab(DevTools::TabDescription const& description, bool) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->reload();
}

void Application::navigate_tab(DevTools::TabDescription const& description, String const& url) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    auto parsed_url = sanitize_url(url, Application::settings().search_engine_settings().engine);
    if (!parsed_url.has_value())
        return;

    view->load(*parsed_url);
}

void Application::traverse_the_history_by_delta(DevTools::TabDescription const& description, int delta) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->traverse_the_history_by_delta(delta);
}

Vector<HTTP::Cookie::Cookie> Application::cookies(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return {};

    return view->session().cookie_jar->get_all_cookies();
}

ErrorOr<void> Application::set_cookie(DevTools::TabDescription const& description, Optional<HTTP::Cookie::Cookie> old_cookie, HTTP::Cookie::Cookie cookie) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return Error::from_string_literal("Unable to locate tab");

    auto url = URL::Parser::basic_parse(description.url);
    if (!url.has_value())
        return Error::from_string_literal("Unable to parse tab URL");

    Optional<CookieStorageKey> old_key;
    if (old_cookie.has_value())
        old_key = CookieStorageKey { old_cookie->name, old_cookie->domain, old_cookie->host_only, old_cookie->path, old_cookie->partition_key };

    TRY(view->session().cookie_jar->set_cookie_from_devtools(*url, move(old_key), move(cookie)));
    return {};
}

void Application::delete_cookies(DevTools::TabDescription const& description, Vector<HTTP::Cookie::Cookie> cookies) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    for (auto const& cookie : cookies)
        view->session().cookie_jar->delete_cookie({ cookie.name, cookie.domain, cookie.host_only, cookie.path, cookie.partition_key });
}

void Application::listen_for_host_cookie_changes(DevTools::TabDescription const& description, OnHostCookieChange on_host_cookie_change) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->listen_for_host_cookie_changes(move(on_host_cookie_change));
}

void Application::stop_listening_for_host_cookie_changes(DevTools::TabDescription const& description) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->stop_listening_for_host_cookie_changes();
}

static ErrorOr<Optional<String>> storage_set_result_to_error_or_old_value(StorageSetResult result)
{
    if (result.has<StorageOperationError>())
        return Error::from_string_literal("Unable to store more than the storage quota");

    auto old_value = result.get<Optional<Utf16String>>();
    if (!old_value.has_value())
        return Optional<String> {};
    return old_value->to_utf8();
}

void Application::inspect_storage(DevTools::TabDescription const& description, Web::StorageAPI::StorageEndpointType storage_endpoint, OnStorageItemsReceived on_complete) const
{
    static u64 next_request_id = 0;

    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    auto request_id = next_request_id++;
    view->on_received_storage_items.set(request_id, move(on_complete));
    view->inspect_storage(storage_endpoint, request_id);
}

// The jar a tab's storage of this kind lives in: session storage belongs to the tab's traversable, local storage to
// its browsing session. Every DevTools edit goes through the jar, like every other write, so the pages mirroring the
// bottle hear of it the same way.
static ErrorOr<StorageJar*> storage_jar_for_devtools(ViewImplementation& view, Web::StorageAPI::StorageEndpointType storage_endpoint)
{
    if (storage_endpoint == Web::StorageAPI::StorageEndpointType::SessionStorage) {
        if (!view.has_traversable())
            return Error::from_string_literal("Unable to locate session storage");
        return &view.traversable().session_storage();
    }
    return view.session().storage_jar.ptr();
}

// The bottle the pages of a DevTools storage host keep their storage in: the host names an origin, and the key is
// that origin's, the way every page names it.
static ErrorOr<String> storage_key_for_devtools_host(String const& host)
{
    auto storage_key = DevTools::storage_key_for_host(host);
    if (!storage_key.has_value())
        return Error::from_string_literal("Storage host is not available");
    return storage_key.release_value();
}

ErrorOr<Optional<String>> Application::set_storage_item(DevTools::TabDescription const& description, Web::StorageAPI::StorageEndpointType storage_endpoint, String const& host, String const& key, String const& value) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return Error::from_string_literal("Unable to locate tab");

    auto* storage_jar = TRY(storage_jar_for_devtools(*view, storage_endpoint));
    auto storage_key = TRY(storage_key_for_devtools_host(host));
    return TRY(storage_set_result_to_error_or_old_value(storage_jar->set_item(storage_endpoint, storage_key, Utf16String::from_utf8(key), Utf16String::from_utf8(value))));
}

ErrorOr<Optional<String>> Application::remove_storage_item(DevTools::TabDescription const& description, Web::StorageAPI::StorageEndpointType storage_endpoint, String const& host, String const& key) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return Error::from_string_literal("Unable to locate tab");

    auto* storage_jar = TRY(storage_jar_for_devtools(*view, storage_endpoint));
    auto storage_key = TRY(storage_key_for_devtools_host(host));
    auto key_utf16 = Utf16String::from_utf8(key);
    auto old_value = storage_jar->get_item(storage_endpoint, storage_key, key_utf16);
    if (!old_value.has_value())
        return Optional<String> {};

    storage_jar->remove_item(storage_endpoint, storage_key, key_utf16);
    return old_value->to_utf8();
}

ErrorOr<void> Application::clear_storage(DevTools::TabDescription const& description, Web::StorageAPI::StorageEndpointType storage_endpoint, String const& host) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return Error::from_string_literal("Unable to locate tab");

    auto* storage_jar = TRY(storage_jar_for_devtools(*view, storage_endpoint));
    auto storage_key = TRY(storage_key_for_devtools_host(host));
    storage_jar->clear_storage_key(storage_endpoint, storage_key);
    return {};
}

u64 Application::add_storage_change_listener(DevTools::TabDescription const& description, OnStorageChange on_storage_change) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        return view->add_storage_change_listener(move(on_storage_change));
    return 0;
}

void Application::remove_storage_change_listener(DevTools::TabDescription const& description, u64 listener_id) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->remove_storage_change_listener(listener_id);
}

void Application::inspect_indexed_database_storage(DevTools::TabDescription const& description, OnIndexedDBInspectionComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->inspect_indexed_database_storage(move(on_complete));
}

void Application::inspect_indexed_database_objects(DevTools::TabDescription const& description, String const& host, Optional<JsonArray> names, JsonObject options, OnIndexedDBInspectionComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->inspect_indexed_database_objects(host, move(names), move(options), move(on_complete));
}

void Application::delete_indexed_database(DevTools::TabDescription const& description, String const& host, String const& name, OnIndexedDBInspectionComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->delete_indexed_database(host, name, move(on_complete));
}

void Application::clear_indexed_database_object_store(DevTools::TabDescription const& description, String const& host, String const& name, OnIndexedDBInspectionComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->clear_indexed_database_object_store(host, name, move(on_complete));
}

void Application::delete_indexed_database_record(DevTools::TabDescription const& description, String const& host, String const& name, OnIndexedDBInspectionComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->delete_indexed_database_record(host, name, move(on_complete));
}

u64 Application::add_indexed_database_change_listener(DevTools::TabDescription const& description, OnIndexedDatabaseChange on_indexed_database_change) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        return view->add_indexed_database_change_listener(move(on_indexed_database_change));
    return 0;
}

void Application::remove_indexed_database_change_listener(DevTools::TabDescription const& description, u64 listener_id) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->remove_indexed_database_change_listener(listener_id);
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

void Application::inspect_dom_node(DevTools::TabDescription const& description, DOMNodeProperties::Type property_type, Web::UniqueNodeID node_id, Optional<Web::CSS::PseudoElement> pseudo_element, JsonObject options) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->inspect_dom_node(node_id, property_type, pseudo_element, JsonValue { move(options) });
}

void Application::inspect_grid_layouts(DevTools::TabDescription const& description, Web::UniqueNodeID root_node_id, OnGridLayoutsReceived on_grid_layouts_received) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_grid_layouts_received({});
        return;
    }

    view->on_received_grid_layouts = [&view = *view, on_grid_layouts_received = move(on_grid_layouts_received)](JsonArray grid_layouts) {
        view.on_received_grid_layouts = nullptr;
        on_grid_layouts_received(move(grid_layouts));
    };

    view->inspect_grid_layouts(root_node_id);
}

void Application::inspect_current_grid(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, OnCurrentGridReceived on_current_grid_received) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_current_grid_received({});
        return;
    }

    view->on_received_current_grid = [&view = *view, on_current_grid_received = move(on_current_grid_received)](Optional<JsonObject> grid_layout) {
        view.on_received_current_grid = nullptr;
        on_current_grid_received(move(grid_layout));
    };

    view->inspect_current_grid(node_id);
}

void Application::inspect_current_flexbox(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, bool only_look_at_parents, OnCurrentFlexboxReceived on_current_flexbox_received) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_current_flexbox_received({});
        return;
    }

    view->on_received_current_flexbox = [&view = *view, on_current_flexbox_received = move(on_current_flexbox_received)](Optional<JsonObject> flexbox_layout) {
        view.on_received_current_flexbox = nullptr;
        on_current_flexbox_received(move(flexbox_layout));
    };

    view->inspect_current_flexbox(node_id, only_look_at_parents);
}

void Application::clear_inspected_dom_node(DevTools::TabDescription const& description) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->clear_inspected_dom_node();
}

void Application::start_node_picker(DevTools::TabDescription const& description, OnNodePickerEvent on_node_picker_event) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->start_node_picker(move(on_node_picker_event));
}

void Application::stop_node_picker(DevTools::TabDescription const& description) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->stop_node_picker();
}

void Application::clear_node_picker(DevTools::TabDescription const& description) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->clear_node_picker();
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

void Application::highlight_flexbox(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, JsonValue options) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->highlight_flexbox(node_id, move(options));
}

void Application::clear_flexbox_highlight(DevTools::TabDescription const& description, Web::UniqueNodeID node_id) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->clear_flexbox_highlight(node_id);
}

void Application::highlight_grid(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, JsonValue options) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->highlight_grid(node_id, move(options));
}

void Application::clear_grid_highlight(DevTools::TabDescription const& description, Web::UniqueNodeID node_id) const
{
    if (auto view = ViewImplementation::find_view_by_id(description.id); view.has_value())
        view->clear_grid_highlight(node_id);
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

void Application::set_dom_node_tag(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, Utf16FlyString const& value, OnDOMNodeEditComplete on_complete) const
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

void Application::replace_dom_node_attribute(DevTools::TabDescription const& description, Web::UniqueNodeID node_id, Utf16FlyString const& name, ReadonlySpan<Attribute> replacement_attributes, OnDOMNodeEditComplete on_complete) const
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

void Application::retrieve_sources(DevTools::TabDescription const& description, OnSourcesReceived on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->retrieve_devtools_sources(move(on_complete));
}

void Application::retrieve_source(DevTools::TabDescription const& description, Web::HTML::ScriptRegistryIdentifier source_id, OnSourceReceived on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->on_received_devtools_source.set(source_id, [on_complete = move(on_complete)](Optional<Web::HTML::ScriptRegistryContent> source) {
        if (!source.has_value()) {
            on_complete(Error::from_string_literal("Unable to locate source"));
            return;
        }

        on_complete(source.release_value());
    });

    view->request_devtools_source(source_id);
}

void Application::listen_for_sources(DevTools::TabDescription const& description, OnSourceAvailable on_source_available) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_devtools_source_available = [on_source_available = move(on_source_available)](Web::HTML::ScriptRegistryDescription source) {
        on_source_available(move(source));
    };
}

void Application::stop_listening_for_sources(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_devtools_source_available = nullptr;
}

void Application::attach_debugger(DevTools::TabDescription const& description, OnDebuggerPaused on_paused, OnDebuggerResumed on_resumed) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;
    view->attach_debugger(move(on_paused), move(on_resumed));
}

void Application::configure_debugger(DevTools::TabDescription const& description, DebuggerConfiguration configuration) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;
    view->configure_debugger(configuration);
}

void Application::detach_debugger(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;
    view->detach_debugger();
}

void Application::interrupt_debugger(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;
    view->interrupt_debugger();
}

void Application::resume_debugger(DevTools::TabDescription const& description, DebuggerResumeMode mode) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;
    view->resume_debugger(mode);
}

void Application::update_debugger_blackboxing(DevTools::TabDescription const& description, Utf16String url, Vector<DebuggerBlackboxRange> ranges, DebuggerBlackboxingOperation operation) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;
    view->update_debugger_blackboxing(move(url), move(ranges), operation);
}

void Application::retrieve_debugger_environments(DevTools::TabDescription const& description, u64 frame_id, OnDebuggerEnvironmentsReceived on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }
    view->retrieve_debugger_environments(frame_id, move(on_complete));
}

void Application::evaluate_javascript_in_debugger_frame(DevTools::TabDescription const& description, u64 frame_id, String const& source_text, OnDebuggerEvaluationComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete("Unable to locate tab"_string);
        return;
    }
    view->evaluate_javascript_in_debugger_frame(frame_id, source_text, move(on_complete));
}

void Application::retrieve_debugger_object_properties(DevTools::TabDescription const& description, u64 object_id, DebuggerObjectScope scope, OnDebuggerObjectPropertiesReceived on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete("Unable to locate tab"_string);
        return;
    }
    view->retrieve_debugger_object_properties(object_id, scope, move(on_complete));
}

void Application::set_debugger_breakpoint(DevTools::TabDescription const& description, DebuggerBreakpointLocation location, DebuggerBreakpointOptions options, OnDebuggerBreakpointOperationComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }
    view->set_debugger_breakpoint(move(location), move(options), move(on_complete));
}

void Application::remove_debugger_breakpoint(DevTools::TabDescription const& description, DebuggerBreakpointLocation location, OnDebuggerBreakpointOperationComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }
    view->remove_debugger_breakpoint(move(location), move(on_complete));
}

void Application::retrieve_debugger_source_positions(DevTools::TabDescription const& description, Web::HTML::ScriptRegistryIdentifier source_id, OnDebuggerSourcePositionsReceived on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }
    view->retrieve_debugger_source_positions(source_id, move(on_complete));
}

void Application::complete_console_input(DevTools::TabDescription const& description, String const& input, OnConsoleInputCompleted on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(DevTools::DevToolsDelegate::ConsoleInputCompletion {});
        return;
    }

    view->complete_js_console_input(input, move(on_complete));
}

void Application::resolve_dom_node_url(DevTools::TabDescription const& description, Optional<Web::UniqueNodeID> node_id, String const& url, OnResolvedURLReceived on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(url);
        return;
    }

    view->resolve_dom_node_url(node_id, url, move(on_complete));
}

void Application::evaluate_javascript(DevTools::TabDescription const& description, String const& script, OnScriptEvaluationComplete on_complete) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value()) {
        on_complete(Error::from_string_literal("Unable to locate tab"));
        return;
    }

    view->on_received_js_console_result = [&view = *view, on_complete = move(on_complete)](DebuggerValue result) {
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

void Application::start_sending_console_output(DevTools::TabDescription const& description, bool messages, bool errors) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->start_sending_console_output(messages, errors);
}

void Application::listen_for_network_events(DevTools::TabDescription const& description, OnNetworkRequestStarted on_request_started, OnNetworkResponseHeadersReceived on_response_headers, OnNetworkResponseBodyReceived on_response_body, OnNetworkRequestFinished on_request_finished) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_network_request_started = [on_request_started = move(on_request_started)](u64 request_id, URL::URL const& url, ByteString const& method, Vector<HTTP::Header> const& headers, ByteBuffer request_body, Optional<String> initiator_type, String referrer_policy, bool is_navigation_request, Web::Fetch::Infrastructure::RequestPriority priority) {
        on_request_started({
            .request_id = request_id,
            .url = url.to_string(),
            .method = MUST(String::from_byte_string(method)),
            .start_time = UnixDateTime::now(),
            .request_headers = headers,
            .request_body = move(request_body),
            .initiator_type = move(initiator_type),
            .referrer_policy = move(referrer_policy),
            .is_navigation_request = is_navigation_request,
            .priority = priority,
        });
    };

    view->on_network_response_headers_received = [on_response_headers = move(on_response_headers)](u64 request_id, u32 status_code, Optional<String> const& reason_phrase, Vector<HTTP::Header> const& headers, Requests::CacheState cache_state) {
        on_response_headers({ request_id, status_code, reason_phrase, headers, cache_state });
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
    listener.on_load_start = [on_started = move(on_started)](URL::URL const& url) {
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

void Application::listen_for_inspect_element_requests(DevTools::TabDescription const& description, OnInspectElementRequest on_inspect_element_request) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_inspect_element_request = move(on_inspect_element_request);
}

void Application::stop_listening_for_inspect_element_requests(DevTools::TabDescription const& description) const
{
    auto view = ViewImplementation::find_view_by_id(description.id);
    if (!view.has_value())
        return;

    view->on_inspect_element_request = nullptr;
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

void Application::did_close_devtools_connection()
{
    m_devtools_client.clear();
}

}
