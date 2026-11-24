/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/Sandbox.h>
#include <AudioToolbox/AudioConverter.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>

namespace AudioServer {

ErrorOr<void> apply_sandbox(StringView mach_server_name)
{
    TRY(Sandbox::configure_runtime());

    auto executable_path = TRY(Core::System::current_executable_path());
    Vector<Sandbox::SeatbeltPath> paths;
    TRY(Sandbox::add_seatbelt_path_if_exists(paths, executable_path, Sandbox::SeatbeltPath::Access::ReadOnly));
    // The audio frameworks look up the main bundle of the process.
    if (auto bundle = Sandbox::application_bundle_for_executable(executable_path); bundle.has_value())
        TRY(Sandbox::add_seatbelt_path_if_exists(paths, *bundle, Sandbox::SeatbeltPath::Access::ReadOnly));

    // AudioToolbox checks the code signature of the process the first time it creates an AudioConverter or initializes
    // an output unit, and it asks for the path behind a descriptor it holds to do so, which the sandbox doesn't allow:
    // The output then fails to open, or the process crashes with SIGBUS. It checks only once per process, and on any
    // attempt to create a converter, even one that fails, so asking for one here gets the check done before the sandbox
    // is in place, whatever the result.
    AudioStreamBasicDescription const source_format {
        .mSampleRate = 48000,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 4,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 4,
        .mChannelsPerFrame = 1,
        .mBitsPerChannel = 32,
        .mReserved = 0,
    };
    auto destination_format = source_format;
    destination_format.mSampleRate = 44100;
    AudioConverterRef converter = nullptr;
    if (AudioConverterNew(&source_format, &destination_format, &converter) == noErr)
        AudioConverterDispose(converter);

    return Sandbox::apply_macos_sandbox({
        .paths = paths.span(),
        .mach_server_name = mach_server_name,
        .system_services = Sandbox::SystemService::Audio,
    });
}

}
