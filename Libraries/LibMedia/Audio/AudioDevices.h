/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
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

// Reports the devices once a list is available and again on every change; an error is a report that could not be made.
using AudioDeviceListCallback = Function<void(ErrorOr<AudioDeviceEnumeration>)>;
void watch_platform_audio_devices(AudioDeviceListCallback);

// The process's view of the audio devices, kept current by watching their source from first use on.
class MEDIA_API AudioDevices {
public:
    AK_ALLOC_WITH_KMALLOC;

    static AudioDevices& the();

    bool has_device_list() const { return m_has_device_list; }
    Vector<AudioDeviceInfo> input_devices() const;
    Vector<AudioDeviceInfo> output_devices() const;

    // The watched source's latest word, which the listeners then hear of.
    void report_device_list(ErrorOr<AudioDeviceEnumeration>);

    using ListenerId = u64;
    ListenerId add_devices_changed_listener(Function<void()>);
    void remove_devices_changed_listener(ListenerId);

private:
    void ensure_watching();
    void notify_listeners();

    Vector<AudioDeviceInfo> m_cached_input_devices;
    Vector<AudioDeviceInfo> m_cached_output_devices;
    bool m_watching { false };
    bool m_has_device_list { false };

    ListenerId m_next_listener_id { 1 };
    HashMap<ListenerId, Function<void()>> m_listeners;
};

}
