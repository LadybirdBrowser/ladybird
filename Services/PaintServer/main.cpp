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
#include <PaintServer/FontResourceCache.h>
#include <PaintServer/Painter.h>
#include <PaintServer/Server.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    StringView mach_server_name;
    bool wait_for_debugger = false;
    u64 server_epoch = 1;
    bool software_painting = false;
    bool force_fontconfig = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(server_epoch, "GPU process epoch", "gpu-epoch", 0, "server_epoch");
    args_parser.add_option(software_painting, "Use software painting and composition", "software-painting");
    args_parser.add_option(force_fontconfig, "Force using fontconfig for font loading", "force-fontconfig");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.parse(arguments);

    PaintServer::set_force_fontconfig(force_fontconfig);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    Core::EventLoop event_loop;

#if defined(AK_OS_MACOS)
    auto ports = TRY(IPC::bootstrap_transport_from_mach_server(mach_server_name));
    auto transport = make<IPC::Transport>(move(ports.receive_right), move(ports.send_right));
#else
    auto socket = TRY(Core::take_over_socket_from_system_server());
    auto transport = make<IPC::Transport>(move(socket));
#endif

    auto painting_mode = software_painting ? PaintServer::Painter::PaintingMode::Software : PaintServer::Painter::PaintingMode::GPU;
    NonnullOwnPtr<PaintServer::Painter> painter = PaintServer::Painter::create(painting_mode);
    PaintServer::Server server(move(painter), server_epoch);
    server.create_broker_connection(move(transport));

    return event_loop.exec();
}
