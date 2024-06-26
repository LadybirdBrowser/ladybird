/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ImageDecoder/ConnectionFromClient.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>

#if defined(AK_OS_MACOS)
#    include <LibCore/Platform/ProcessStatisticsMach.h>
#endif

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    Core::ArgsParser args_parser;
    StringView mach_server_name;
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.parse(arguments);

    Core::EventLoop event_loop;

#if defined(AK_OS_MACOS)
    if (!mach_server_name.is_empty())
        Core::Platform::register_with_mach_server(mach_server_name);
#endif

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<ImageDecoder::ConnectionFromClient>());

    return event_loop.exec();
}
