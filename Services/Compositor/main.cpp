/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ConnectionFromClient.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/PathFontProvider.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <LibWebView/Utilities.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    StringView mach_server_name;
    bool wait_for_debugger = false;
    bool force_cpu_painting = false;
    bool force_fontconfig = false;
    bool disable_async_scrolling = false;

    Core::ArgsParser args_parser;
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(force_cpu_painting, "Force CPU painting", "force-cpu-painting");
    args_parser.add_option(force_fontconfig, "Force using fontconfig for font loading", "force-fontconfig");
    args_parser.add_option(disable_async_scrolling, "Disable async scrolling", "disable-async-scrolling");
    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    WebView::platform_init();
    auto& font_provider = static_cast<Gfx::PathFontProvider&>(Gfx::FontDatabase::the().install_system_font_provider(make<Gfx::PathFontProvider>()));
    if (force_fontconfig)
        font_provider.set_name_but_fixme_should_create_custom_system_font_provider("FontConfig"_string);

    if (!force_cpu_painting)
        Gfx::SkiaBackendContext::initialize_gpu_backend();
    auto skia_backend_context = Gfx::SkiaBackendContext::the_main_thread_context();

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();
    auto client = TRY(IPC::take_over_accepted_client_from_system_server<Compositor::ConnectionFromClient>(
        mach_server_name, move(skia_backend_context), !disable_async_scrolling));

    return event_loop.exec();
}
