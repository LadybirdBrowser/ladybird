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
#include <LibCore/System.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Resolver.h>
#include <RequestServer/ResourceSubstitutionMap.h>

#if defined(AK_OS_MACOS)
#    include <LibCore/Platform/ProcessStatisticsMach.h>
#endif

namespace RequestServer {

extern Optional<HTTP::DiskCache> g_disk_cache;
OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

}

#ifndef AK_OS_WINDOWS
static void handle_signal(int signal)
{
    VERIFY(signal == SIGINT || signal == SIGTERM);
    Core::EventLoop::current().quit(0);
}
#endif

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    Vector<ByteString> certificates;
    StringView mach_server_name;
    StringView http_disk_cache_mode;
    StringView resource_map_path;
    bool wait_for_debugger = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(certificates, "Path to a certificate file", "certificate", 'C', "certificate");
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(http_disk_cache_mode, "HTTP disk cache mode", "http-disk-cache-mode", 0, "mode");
    args_parser.add_option(resource_map_path, "Path to JSON file mapping URLs to local files", "resource-map", 0, "path");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    // FIXME: Update RequestServer to support multiple custom root certificates.
    if (!certificates.is_empty())
        RequestServer::set_default_certificate_path(certificates.first());

    if (!resource_map_path.is_empty()) {
        auto map = RequestServer::ResourceSubstitutionMap::load_from_file(resource_map_path);
        if (map.is_error())
            warnln("Unable to load resource substitution map from '{}': {}", resource_map_path, map.error());
        else
            RequestServer::g_resource_substitution_map = map.release_value();
    }

#if !defined(AK_OS_WINDOWS)
    MUST(Core::System::signal(SIGPIPE, SIG_IGN));
#endif

    Core::EventLoop event_loop;
    // FIXME: Have another way to signal the event loop to gracefully quit on windows.
#ifndef AK_OS_WINDOWS
    Core::EventLoop::register_signal(SIGINT, handle_signal);
    Core::EventLoop::register_signal(SIGTERM, handle_signal);
#endif

#if defined(AK_OS_MACOS)
    if (!mach_server_name.is_empty())
        Core::Platform::register_with_mach_server(mach_server_name);
#endif

    if (http_disk_cache_mode != "disabled"sv) {
        auto mode = TRY([&]() -> ErrorOr<HTTP::DiskCache::Mode> {
            if (http_disk_cache_mode == "enabled"sv)
                return HTTP::DiskCache::Mode::Normal;
            if (http_disk_cache_mode == "partitioned"sv)
                return HTTP::DiskCache::Mode::Partitioned;
            if (http_disk_cache_mode == "testing"sv)
                return HTTP::DiskCache::Mode::Testing;
            return Error::from_string_literal("Unrecognized disk cache mode");
        }());

        if (auto cache = HTTP::DiskCache::create(mode); cache.is_error())
            warnln("Unable to create disk cache: {}", cache.error());
        else
            RequestServer::g_disk_cache = cache.release_value();
    }

    // Connections are stored on the stack to ensure they are destroyed before
    // static destruction begins. This prevents crashes from notifiers trying to
    // unregister from already-destroyed thread data during process exit.
    HashMap<int, NonnullRefPtr<RequestServer::ConnectionFromClient>> connections;
    RequestServer::ConnectionFromClient::set_connections(connections);

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<RequestServer::ConnectionFromClient>());
    client->mark_as_primary_connection();

    return event_loop.exec();
}
