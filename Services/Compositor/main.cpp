/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/ConnectionFromClient.h>
#include <Compositor/FontSkia.h>
#include <Compositor/Sandbox.h>
#include <Compositor/ScrollbarController.h>
#include <Compositor/SkiaBackendContext.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Platform/TaskRole.h>
#include <LibCore/Platform/ThreadQoS.h>
#include <LibCore/Process.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/StatsLog.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/GpuDeviceIdentity.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    StringView mach_server_name;
    StringView cache_path;
    StringView resource_root;
    bool wait_for_debugger = false;
    bool enable_test_mode = false;
    bool force_cpu_painting = false;
    bool force_fontconfig = false;
    bool disable_sandbox = false;

    int crash_report_fd = -1;
    StringView stats_log_directory;
    Core::ArgsParser args_parser;
    args_parser.add_option(stats_log_directory, "Directory for process statistics (JSON lines)", "stats-log-dir", 0, "path");
    args_parser.add_option(crash_report_fd, "Descriptor for anonymous crash diagnostics", "crash-report-fd", 0, "fd");
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(cache_path, "Path to the profile cache", "cache-path", 0, "path");
    args_parser.add_option(resource_root, "Path to the browser's resources", "resource-root", 0, "path");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(enable_test_mode, "Enable test mode", "test-mode");
    args_parser.add_option(force_cpu_painting, "Force CPU painting", "force-cpu-painting");
    args_parser.add_option(force_fontconfig, "Force using fontconfig for font loading", "force-fontconfig");
    args_parser.add_option(disable_sandbox, "Disable process sandboxing", "disable-sandbox");
    args_parser.parse(arguments);

    auto stats_log = TRY(Core::StatsLog::create(stats_log_directory, "Compositor"sv));

    if (resource_root.is_empty())
        return Error::from_string_literal("--resource-root is required");

    if (crash_report_fd >= 0) {
        if (auto result = Core::CrashHandler::initialize(crash_report_fd); result.is_error())
            warnln("Could not install crash report handler: {}", result.error());
    }

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    if (auto result = Core::Platform::adopt_foreground_application_task_role(); result.is_error())
        warnln("Could not adopt the foreground application task role: {}", result.error());
    if (auto result = Core::Platform::set_current_thread_qos(Core::Platform::ThreadQoS::UserInteractive); result.is_error())
        warnln("Could not set main thread QoS: {}", result.error());

    if (enable_test_mode) {
        Compositor::force_font_hinting_for_testing();
        Compositor::ScrollbarController::show_scrollbars_always_for_testing();
    }

    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(TRY(String::from_utf8(resource_root))));
    if (force_fontconfig)
        Compositor::set_font_rasterizer(Compositor::FontRasterizer::FreeType);

#if defined(AK_OS_LINUX)
    // NB: The GPU driver starts threads, and Landlock does not confine a thread that already runs. Seccomp covers every
    //     thread, so the rest of the sandbox can wait until the driver is done with the syscalls that only its setup uses.
    if (!disable_sandbox)
        TRY(Compositor::restrict_filesystem(resource_root));
#endif

    if (!force_cpu_painting)
        Gfx::SkiaBackendContext::initialize_gpu_backend();
    auto skia_backend_context = Gfx::SkiaBackendContext::the_main_thread_context();

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();

    if (!disable_sandbox)
        TRY(Compositor::apply_sandbox(mach_server_name, cache_path, resource_root));

    Optional<Gfx::GpuDeviceIdentity> gpu_device_identity;
#ifdef USE_VULKAN_DMABUF_IMAGES
    if (skia_backend_context && skia_backend_context->vulkan_context().can_sample_through_ycbcr_conversion)
        gpu_device_identity = skia_backend_context->vulkan_context().device_identity;
#endif

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<Compositor::ConnectionFromClient>(
        mach_server_name, move(skia_backend_context)));
    client->async_gpu_device_identity_reported(move(gpu_device_identity));

    if (stats_log)
        stats_log->start_sampling([](JsonObject& values) {
            if (auto context = Gfx::SkiaBackendContext::the_main_thread_context())
                values.set("gpu.skia_resource_cache_bytes"sv, context->resource_cache_bytes());
        });

    return event_loop.exec();
}
