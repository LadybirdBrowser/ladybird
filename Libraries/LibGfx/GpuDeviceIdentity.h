/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibIPC/Forward.h>

namespace Gfx {

// Names a GPU across processes, since memory that one process shares from a GPU can only be imported on the same one.
// The bytes are whatever the graphics API identifies the device and its driver by, and are only compared.
struct GpuDeviceIdentity {
    static constexpr size_t MAXIMUM_BYTE_COUNT = 64;

    ByteBuffer bytes;

    bool operator==(GpuDeviceIdentity const&) const = default;

    ByteString to_byte_string() const;
    static Optional<GpuDeviceIdentity> from_string(StringView);
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::GpuDeviceIdentity const&);

template<>
ErrorOr<Gfx::GpuDeviceIdentity> decode(Decoder&);

}
