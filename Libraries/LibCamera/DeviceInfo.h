/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibGC/Forward.h>
#include <LibIPC/Forward.h>

namespace Camera {

struct DeviceInfo {
    ByteString dom_device_id;
    ByteString label;
    ByteString group_id;
    bool is_default { false };
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Camera::DeviceInfo const&);

template<>
ErrorOr<Camera::DeviceInfo> decode(Decoder&);

}

GC_UNTRACED_CAPTURE(Camera::DeviceInfo);
