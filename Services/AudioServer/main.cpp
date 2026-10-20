/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/BrokeredAudioDaemon.h>
#include <AudioServer/ControlConnection.h>
#include <AudioServer/PlatformAudio.h>
#include <AudioServer/Sandbox.h>
#include <AudioServer/Tabs.h>
#include <LibAudio/AudioDevices.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Platform/TaskRole.h>
#include <LibCore/Platform/ThreadQoS.h>
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
    args_parser.add_option(is_headless, "Discard the mixed output instead of playing it", "headless");
    args_parser.parse(arguments);

    auto stats_log = TRY(Core::StatsLog::create(stats_log_directory, "AudioServer"sv));

    if (crash_report_fd >= 0) {
        if (auto result = Core::CrashHandler::initialize(crash_report_fd); result.is_error())
            warnln("Could not install crash report handler: {}", result.error());
    }

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    if (auto result = Core::Platform::adopt_foreground_application_task_role(); result.is_error())
        warnln("Could not adopt the foreground application task role: {}", result.error());
    if (auto result = Core::Platform::set_current_thread_qos(Core::Platform::ThreadQoS::UserInitiated); result.is_error())
        warnln("Could not set main thread QoS: {}", result.error());

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();

#if defined(AK_OS_LINUX)
    // Without the sandbox there is no redirect to the broker, so libpulse finds the daemon the usual way.
    if (connect_broker_fd != -1 && !disable_sandbox) {
        Sandbox::set_connect_broker_fd(connect_broker_fd);
        // The broker connects this name to the audio daemon, so libpulse neither looks for one nor starts one.
        TRY(Core::Environment::set("PULSE_SERVER"sv, AudioServer::brokered_audio_daemon_path, Core::Environment::Overwrite::Yes));
    }
#endif

    if (!disable_sandbox)
        TRY(AudioServer::apply_sandbox(mach_server_name));

    if (is_headless) {
        AudioServer::Tabs::the().set_audio_output(Audio::AudioOutput::Null);
    } else {
        // The watch reports changes on the running loop, so it starts once there is one.
        event_loop.deferred_invoke([] {
            Audio::watch_platform_audio_devices([](ErrorOr<Audio::AudioDeviceEnumeration> devices) {
                Audio::AudioDevices::the().report_device_list(move(devices));
            });
        });
    }

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<AudioServer::ControlConnection>(mach_server_name));
    AudioServer::Tabs::the().set_control_connection(*client);

    if (stats_log)
        stats_log->start_sampling();

    return event_loop.exec();
}
