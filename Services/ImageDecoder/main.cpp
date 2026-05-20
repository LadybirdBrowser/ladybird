/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ImageDecoder/ConnectionFromClient.h>
#include <ImageDecoder/Sandbox.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    Core::ArgsParser args_parser;
    StringView mach_server_name;
    bool wait_for_debugger = false;
    bool enable_sandbox = false;

    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(enable_sandbox, "Enable process sandboxing", "enable-sandbox");
    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    if (enable_sandbox)
        TRY(ImageDecoder::apply_sandbox());

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<ImageDecoder::ConnectionFromClient>(mach_server_name));

    return event_loop.exec();
}
