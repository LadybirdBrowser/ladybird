/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Resolver.h>

#if defined(AK_OS_MACOS)
#    include <LibCore/Platform/ProcessStatisticsMach.h>
#endif

namespace RequestServer {

extern Optional<HTTP::DiskCache> g_disk_cache;

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    Vector<ByteString> certificates;
    StringView mach_server_name;
    StringView http_disk_cache_mode;
    bool wait_for_debugger = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(http_disk_cache_mode, "HTTP disk cache mode", "http-disk-cache-mode", 0, "mode");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    // FIXME: Update RequestServer to support multiple custom root certificates.
    if (!certificates.is_empty())
        RequestServer::set_default_certificate_path(certificates.first());

    Core::EventLoop event_loop;

#if defined(AK_OS_MACOS)
    if (!mach_server_name.is_empty())
        Core::Platform::register_with_mach_server(mach_server_name);
#endif

    if (http_disk_cache_mode.is_one_of("enabled"sv, "testing"sv)) {
        auto mode = http_disk_cache_mode == "enabled"sv
            ? HTTP::DiskCache::Mode::Normal
            : HTTP::DiskCache::Mode::Testing;

        if (auto cache = HTTP::DiskCache::create(mode); cache.is_error())
            warnln("Unable to create disk cache: {}", cache.error());
        else
            RequestServer::g_disk_cache = cache.release_value();
    }

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<RequestServer::ConnectionFromClient>());

    return event_loop.exec();
}
