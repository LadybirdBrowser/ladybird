/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/SourceLocation.h>
#include <AK/Vector.h>
#include <CoreAudio/CoreAudioTypes.h>
#include <LibMedia/Audio/ChannelMap.h>

namespace AudioServer {

Audio::ChannelMap device_channel_layout(AudioChannelLayout const& channel_layout, u32 layout_size, u32 expected_channel_count);

inline void log_os_error_code([[maybe_unused]] OSStatus error_code, [[maybe_unused]] SourceLocation location = SourceLocation::current())
{
#if AUDIO_DEBUG
    auto error_string = "Unknown error"sv;

    // Errors listed in AUComponent.h
    switch (error_code) {
    case kAudioUnitErr_InvalidProperty:
        error_string = "InvalidProperty"sv;
        break;
    case kAudioUnitErr_InvalidParameter:
        error_string = "InvalidParameter"sv;
        break;
    case kAudioUnitErr_InvalidElement:
        error_string = "InvalidElement"sv;
        break;
    case kAudioUnitErr_NoConnection:
        error_string = "NoConnection"sv;
        break;
    case kAudioUnitErr_FailedInitialization:
        error_string = "FailedInitialization"sv;
        break;
    case kAudioUnitErr_TooManyFramesToProcess:
        error_string = "TooManyFramesToProcess"sv;
        break;
    case kAudioUnitErr_InvalidFile:
        error_string = "InvalidFile"sv;
        break;
    case kAudioUnitErr_UnknownFileType:
        error_string = "UnknownFileType"sv;
        break;
    case kAudioUnitErr_FileNotSpecified:
        error_string = "FileNotSpecified"sv;
        break;
    case kAudioUnitErr_FormatNotSupported:
        error_string = "FormatNotSupported"sv;
        break;
    case kAudioUnitErr_Uninitialized:
        error_string = "Uninitialized"sv;
        break;
    case kAudioUnitErr_InvalidScope:
        error_string = "InvalidScope"sv;
        break;
    case kAudioUnitErr_PropertyNotWritable:
        error_string = "PropertyNotWritable"sv;
        break;
    case kAudioUnitErr_CannotDoInCurrentContext:
        error_string = "CannotDoInCurrentContext"sv;
        break;
    case kAudioUnitErr_InvalidPropertyValue:
        error_string = "InvalidPropertyValue"sv;
        break;
    case kAudioUnitErr_PropertyNotInUse:
        error_string = "PropertyNotInUse"sv;
        break;
    case kAudioUnitErr_Initialized:
        error_string = "Initialized"sv;
        break;
    case kAudioUnitErr_InvalidOfflineRender:
        error_string = "InvalidOfflineRender"sv;
        break;
    case kAudioUnitErr_Unauthorized:
        error_string = "Unauthorized"sv;
        break;
    case kAudioUnitErr_MIDIOutputBufferFull:
        error_string = "MIDIOutputBufferFull"sv;
        break;
    case kAudioComponentErr_InstanceTimedOut:
        error_string = "InstanceTimedOut"sv;
        break;
    case kAudioComponentErr_InstanceInvalidated:
        error_string = "InstanceInvalidated"sv;
        break;
    case kAudioUnitErr_RenderTimeout:
        error_string = "RenderTimeout"sv;
        break;
    case kAudioUnitErr_ExtensionNotFound:
        error_string = "ExtensionNotFound"sv;
        break;
    case kAudioUnitErr_InvalidParameterValue:
        error_string = "InvalidParameterValue"sv;
        break;
    case kAudioUnitErr_InvalidFilePath:
        error_string = "InvalidFilePath"sv;
        break;
    case kAudioUnitErr_MissingKey:
        error_string = "MissingKey"sv;
        break;
    default:
        break;
    }

    warnln("{}: Audio Unit error {}: {}", location, error_code, error_string);
#endif
}

} // namespace AudioServer
