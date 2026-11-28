/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <CameraServer/BrokeredCameraDaemon.h>
#include <CameraServer/CameraBackend.h>
#include <CameraServer/ControlConnection.h>
#include <CameraServer/FrameConversion.h>
#include <CameraServer/Sandbox.h>
#include <CameraServer/Tabs.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/StatsLog.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <LibSandbox/ConnectBroker.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    int crash_report_fd = -1;
    int connect_broker_fd = -1;
    StringView mach_server_name;
    bool wait_for_debugger = false;
    bool disable_sandbox = false;
    bool is_headless = false;

    StringView stats_log_directory;
    Core::ArgsParser args_parser;
    args_parser.add_option(stats_log_directory, "Directory for process statistics (JSON lines)", "stats-log-dir", 0, "path");
    args_parser.add_option(crash_report_fd, "Descriptor for anonymous crash diagnostics", "crash-report-fd", 0, "fd");
    args_parser.add_option(connect_broker_fd, "Descriptor for the sandbox connection broker", "connect-broker-fd", 0, "fd");
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(disable_sandbox, "Disable process sandboxing", "disable-sandbox");
    args_parser.add_option(is_headless, "Give a synthetic camera instead of the platform's", "headless");
    args_parser.parse(arguments);

    auto stats_log = TRY(Core::StatsLog::create(stats_log_directory, "ladybird-camera"sv));

    if (crash_report_fd >= 0) {
        if (auto result = Core::CrashHandler::initialize(crash_report_fd); result.is_error())
            warnln("Could not install crash report handler: {}", result.error());
    }

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();

#if defined(AK_OS_LINUX)
    // Without the sandbox there is no redirect to the broker, so the camera library finds the daemon the usual way.
    if (connect_broker_fd != -1 && !disable_sandbox) {
        Sandbox::set_connect_broker_fd(connect_broker_fd);
        // The broker connects this name to the daemon, so the library looks nowhere else for one.
        TRY(Core::Environment::set("PIPEWIRE_REMOTE"sv, CameraServer::brokered_camera_daemon_path, Core::Environment::Overwrite::Yes));
    }
#endif

    // Load backend resources before sandboxing; cameras are opened only when a client asks.
    CameraServer::prepare_frame_conversion();
    CameraServer::Tabs::the().set_backend(is_headless ? CameraServer::create_synthetic_camera_backend() : CameraServer::create_platform_camera_backend());

    if (!disable_sandbox)
        TRY(CameraServer::apply_sandbox(mach_server_name));

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<CameraServer::ControlConnection>(mach_server_name));
    CameraServer::Tabs::the().set_control_connection(*client);

    if (stats_log)
        stats_log->start_sampling();

    return event_loop.exec();
}
