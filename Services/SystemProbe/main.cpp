/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibGfx/GpuDeviceIdentity.h>
#include <LibMain/Main.h>
#include <LibPlatformFeatures/Build.h>
#include <LibPlatformFeatures/SystemFacts.h>
#include <SystemProbe/FFmpegDescription.h>
#include <SystemProbe/VulkanDescription.h>

// Describes the system for the Browser's feature blocklist, and exits. It takes nothing but its arguments and writes
// nothing but its report, so that it can run with the GPU driver loaded and no sandbox.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView gpu_device;

    Core::ArgsParser args_parser;
    args_parser.add_option(gpu_device, "The GPU that presents, as the presentation identifies it", "gpu-device", 0, "identity");
    args_parser.parse(arguments);

    Optional<Gfx::GpuDeviceIdentity> preferred_device;
    if (!gpu_device.is_empty()) {
        preferred_device = Gfx::GpuDeviceIdentity::from_string(gpu_device);
        if (!preferred_device.has_value())
            return Error::from_string_literal("Malformed GPU device identity");
    }

    PlatformFeatures::SystemFacts facts;
    if constexpr (PlatformFeatures::this_build.paints_through_vulkan)
        facts.gpu = describe_gpu_through_vulkan(preferred_device);
    if constexpr (PlatformFeatures::this_build.decodes_video_through_vulkan()) {
        facts.bundled_ffmpeg = describe_bundled_ffmpeg();
        facts.system_ffmpeg = describe_system_ffmpeg();
    }

    auto report = facts.to_json().serialized();
    auto output = TRY(Core::File::standard_output());
    TRY(output->write_until_depleted(report.bytes()));

    // Closing the output ends the report, so the Browser does not wait for the driver to unload as the probe exits.
    output->close();
    return 0;
}
