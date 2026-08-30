/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ConnectionFromClient.h>
#include <Compositor/Sandbox.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <LibWebView/Utilities.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    StringView mach_server_name;
    StringView cache_path;
    bool wait_for_debugger = false;
    bool enable_test_mode = false;
    bool force_cpu_painting = false;
    bool force_fontconfig = false;
    bool disable_async_scrolling = false;
    bool disable_sandbox = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(cache_path, "Path to the profile cache", "cache-path", 0, "path");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(enable_test_mode, "Enable test mode", "test-mode");
    args_parser.add_option(force_cpu_painting, "Force CPU painting", "force-cpu-painting");
    args_parser.add_option(force_fontconfig, "Force using fontconfig for font loading", "force-fontconfig");
    args_parser.add_option(disable_async_scrolling, "Disable async scrolling", "disable-async-scrolling");
    args_parser.add_option(disable_sandbox, "Disable process sandboxing", "disable-sandbox");
    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    if (enable_test_mode)
        Gfx::force_hinting_for_testing(Gfx::FontHintingStyle::Normal);

    WebView::platform_init();
    if (force_fontconfig)
        Gfx::FontDatabase::the().set_force_freetype_rasterization(true);

    if (!force_cpu_painting)
        Gfx::SkiaBackendContext::initialize_gpu_backend();
    auto skia_backend_context = Gfx::SkiaBackendContext::the_main_thread_context();

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();

    if (!disable_sandbox)
        TRY(Compositor::apply_sandbox(cache_path));

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<Compositor::ConnectionFromClient>(
        mach_server_name, move(skia_backend_context), !disable_async_scrolling));

    return event_loop.exec();
}
