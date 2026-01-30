/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudioServerClient/Client.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/EventLoopPluginSerenity.h>
#include <LibWeb/WebAudio/Debug.h>
#include <WebAudioWorker/WebAudioWorkerConnection.h>
#if defined(AK_OS_MACOS)
#    include <LibCore/Platform/ProcessStatisticsMach.h>
#endif

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    bool wait_for_debugger = false;
    StringView mach_server_name;
    int audio_server_socket = -1;

    Core::ArgsParser args_parser;
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(audio_server_socket, "File descriptor of the socket for the AudioServer connection", "audio-server-socket", 0, "audio_server_socket");
    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    Core::EventLoop event_loop;

    Web::WebAudio::mark_current_thread_as_control_thread();

    Web::Platform::EventLoopPlugin::install(*new Web::Platform::EventLoopPluginSerenity);

    if (audio_server_socket != -1) {
        auto socket = TRY(Core::LocalSocket::adopt_fd(audio_server_socket));
        TRY(socket->set_blocking(true));
        auto client = TRY(try_make_ref_counted<AudioServerClient::Client>(make<IPC::Transport>(move(socket))));
        AudioServerClient::Client::set_default_client(client);
    }

#if defined(AK_OS_MACOS)
    if (!mach_server_name.is_empty())
        Core::Platform::register_with_mach_server(mach_server_name);
#endif

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<WebAudioWorker::WebAudioWorkerConnection>());

    return event_loop.exec();
}
