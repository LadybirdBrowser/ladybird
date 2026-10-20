/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Platform/TaskRole.h>
#include <LibCore/Platform/ThreadQoS.h>
#include <LibCore/Process.h>
#include <LibCore/StatsLog.h>
#include <LibGfx/GpuDeviceIdentity.h>
#include <LibIPC/SingleServer.h>
#include <LibMain/Main.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMediaCodecs/HardwareVideoDecoders.h>
#include <LibMediaCodecs/VideoDecoderRegistry.h>
#include <LibMediaCodecs/Vulkan/VulkanVideoDevice.h>
#include <VideoDecoderServer/ControlConnection.h>
#include <VideoDecoderServer/Sandbox.h>

#if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
#    include <LibMediaCodecs/FFmpeg/SystemFFmpeg.h>
#endif

static Media::DecoderErrorOr<NonnullOwnPtr<Media::VideoDecoder>> create_software_video_decoder(Media::CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    return NonnullOwnPtr<Media::VideoDecoder> { TRY(Media::FFmpeg::FFmpegVideoDecoder::try_create(codec_id, codec_initialization_data)) };
}

// Software decoders belong in the MediaServer, away from the GPU access this process will have. These are only for
// exercising the path through this process where no hardware decoder is available, as in tests.
static ReadonlySpan<Media::VideoDecoderRegistration> software_video_decoders_for_testing()
{
    static Array const decoders {
        Media::VideoDecoderRegistration { Media::FFmpeg::FFmpegVideoDecoder::capabilities, create_software_video_decoder },
    };
    return decoders;
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    int crash_report_fd = -1;
    StringView stats_log_directory;
    StringView mach_server_name;
    bool wait_for_debugger = false;
    bool disable_sandbox = false;
    bool host_software_decoders = false;
    StringView gpu_device;

    Core::ArgsParser args_parser;
    args_parser.add_option(stats_log_directory, "Directory for process statistics logs", "stats-log-dir", 0, "directory");
    args_parser.add_option(crash_report_fd, "Descriptor for anonymous crash diagnostics", "crash-report-fd", 0, "fd");
    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(disable_sandbox, "Disable process sandboxing", "disable-sandbox");
    args_parser.add_option(host_software_decoders, "Host software decoders, for testing", "host-software-decoders");
    args_parser.add_option(gpu_device, "The GPU to decode on, as the Compositor identifies it", "gpu-device", 0, "identity");
    args_parser.parse(arguments);

    if (crash_report_fd >= 0) {
        if (auto result = Core::CrashHandler::initialize(crash_report_fd); result.is_error())
            warnln("Could not install crash report handler: {}", result.error());
    }

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    if (auto result = Core::Platform::adopt_foreground_application_task_role(); result.is_error())
        warnln("Could not adopt the foreground application task role: {}", result.error());
    if (auto result = Core::Platform::set_current_thread_qos(Core::Platform::ThreadQoS::UserInitiated); result.is_error())
        warnln("Could not set main thread QoS: {}", result.error());

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();
    auto stats_log = TRY(Core::StatsLog::create(stats_log_directory, "VideoDecoderServer"sv));

    Optional<Gfx::GpuDeviceIdentity> gpu_device_identity;
    if (!gpu_device.is_empty()) {
        gpu_device_identity = Gfx::GpuDeviceIdentity::from_string(gpu_device);
        if (!gpu_device_identity.has_value())
            return Error::from_string_literal("Malformed GPU device identity");
    }

#if defined(AK_OS_LINUX)
    if (!disable_sandbox)
        TRY(VideoDecoderServer::restrict_filesystem(gpu_device_identity.has_value()));
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
    // The driver loads before the rest of the sandbox applies. Without a device, the GPU's decoders go unused.
    if (gpu_device_identity.has_value()) {
        if (auto result = Media::VulkanVideoDevice::create_for_process(*gpu_device_identity); result.is_error())
            warnln("Could not create a Vulkan device for decoding video: {}", result.error());
    }
#endif

#if defined(USE_VULKAN_DMABUF_IMAGES) && defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
    // H.264 and HEVC decode through the system library, whose dependencies make syscalls the sandbox will not allow.
    if (Media::VulkanVideoDevice::the() != nullptr)
        (void)Media::FFmpeg::SystemFFmpeg::the();
#endif

    Vector<Media::VideoDecoderRegistration> hosted_decoders;
    auto hardware_decoders = Media::hardware_video_decoders();
    hosted_decoders.append(hardware_decoders.data(), hardware_decoders.size());
    if (host_software_decoders) {
        auto software_decoders = software_video_decoders_for_testing();
        hosted_decoders.append(software_decoders.data(), software_decoders.size());
    }

    if (!disable_sandbox)
        TRY(VideoDecoderServer::apply_sandbox(mach_server_name));

    auto client = TRY(IPC::take_over_accepted_client_from_system_server<VideoDecoderServer::ControlConnection>(mach_server_name, hosted_decoders));

    if (stats_log)
        stats_log->start_sampling();

    return event_loop.exec();
}
