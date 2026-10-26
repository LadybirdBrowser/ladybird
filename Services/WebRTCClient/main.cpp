/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/Process.h>
#include <LibMain/Main.h>
#include <WebRTCClient/Sandbox.h>

extern "C" int webrtc_client_run(u8 const* mach_server_name, size_t mach_server_name_length, bool network_access_allowed);

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    int crash_report_fd = -1;
    StringView mach_server_name;
    bool wait_for_debugger = false;
    bool disable_sandbox = false;
    bool disable_network = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(crash_report_fd, "Descriptor for anonymous crash diagnostics", "crash-report-fd", 0, "fd");
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(disable_sandbox, "Disable process sandboxing", "disable-sandbox");
    args_parser.add_option(disable_network, "Disable network access, as when a proxy is configured", "disable-network");
    args_parser.parse(arguments);

    if (crash_report_fd >= 0) {
        if (auto result = Core::CrashHandler::initialize(crash_report_fd); result.is_error())
            warnln("Could not install crash report handler: {}", result.error());
    }

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    // NB: The sandbox goes in before the Rust side starts its runtime, since Landlock only confines threads that start
    //     after it is applied.
    auto network_access = disable_network ? WebRTCClient::NetworkAccess::Denied : WebRTCClient::NetworkAccess::Allowed;
    if (!disable_sandbox)
        TRY(WebRTCClient::apply_sandbox(mach_server_name, network_access));

    return webrtc_client_run(reinterpret_cast<u8 const*>(mach_server_name.characters_without_null_termination()), mach_server_name.length(), network_access == WebRTCClient::NetworkAccess::Allowed);
}
