/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibCore/Promise.h>
#include <LibMedia/Export.h>

namespace Media {

struct AudioDeviceInfo {
    ByteString dom_device_id;
    ByteString label;
    ByteString group_id;
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    bool is_default { false };
};

struct AudioDeviceEnumeration {
    Vector<AudioDeviceInfo> inputs;
    Vector<AudioDeviceInfo> outputs;
};

using AudioDeviceEnumerationPromise = Core::Promise<AudioDeviceEnumeration>;

NonnullRefPtr<AudioDeviceEnumerationPromise> enumerate_platform_audio_devices();

class MEDIA_API AudioDevices {
public:
    static AudioDevices& the();

    void refresh();
    bool has_completed_refresh() const { return m_has_completed_refresh; }
    Vector<AudioDeviceInfo> input_devices() const;
    Vector<AudioDeviceInfo> output_devices() const;

    using ListenerId = u64;
    ListenerId add_devices_changed_listener(Function<void()>);
    void remove_devices_changed_listener(ListenerId);

private:
    void notify_listeners();

    Vector<AudioDeviceInfo> m_cached_input_devices;
    Vector<AudioDeviceInfo> m_cached_output_devices;
    bool m_refresh_in_progress { false };
    bool m_has_completed_refresh { false };

    ListenerId m_next_listener_id { 1 };
    HashMap<ListenerId, Function<void()>> m_listeners;
};

}
