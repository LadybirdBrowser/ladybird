/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWebAudio/Debug.h>
#include <WebAudioWorker/MainEventLoop.h>
#include <WebAudioWorker/WebAudioBrokerConnection.h>
#if defined(AK_OS_MACOS)
#    include <LibCore/Platform/ProcessStatisticsMach.h>
#endif

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    bool wait_for_debugger = false;
    StringView mach_server_name;

    Core::ArgsParser args_parser;
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    Core::EventLoop event_loop;
    auto main_event_loop = Core::EventLoop::current_weak();
    Web::WebAudio::set_main_event_loop_reference(move(main_event_loop));

    Web::WebAudio::mark_current_thread_as_control_thread();

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPlugin);

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<Web::WebAudio::WebAudioBrokerConnection>(
        mach_server_name));

    return event_loop.exec();
}
