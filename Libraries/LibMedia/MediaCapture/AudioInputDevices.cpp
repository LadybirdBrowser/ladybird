/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudioServerClient/Client.h>
#include <LibMedia/MediaCapture/AudioInputDevices.h>

namespace Media::Capture {

ErrorOr<Vector<AudioInputDeviceInfo>> AudioInputDevices::enumerate()
{
    auto client = AudioServerClient::Client::default_client();
    if (!client)
        return Error::from_string_literal("MediaCapture: no AudioServer client available");

    auto devices_or_error = client->get_audio_input_devices();
    if (devices_or_error.is_error())
        return devices_or_error.release_error();

    auto devices = devices_or_error.release_value();
    Vector<AudioInputDeviceInfo> result;
    result.ensure_capacity(devices.size());

    for (auto const& device : devices) {
        result.append(AudioInputDeviceInfo {
            .device_id = device.device_id,
            .label = device.label,
            .persistent_id = device.persistent_id,
            .sample_rate_hz = device.sample_rate_hz,
            .channel_count = device.channel_count,
            .is_default = device.is_default,
        });
    }

    return result;
}

}
