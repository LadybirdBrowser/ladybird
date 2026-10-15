/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <LibGfx/GpuDeviceIdentity.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

ByteString GpuDeviceIdentity::to_byte_string() const
{
    return encode_hex(bytes);
}

Optional<GpuDeviceIdentity> GpuDeviceIdentity::from_string(StringView string)
{
    auto bytes = decode_hex(string);
    if (bytes.is_error() || bytes.value().is_empty() || bytes.value().size() > MAXIMUM_BYTE_COUNT)
        return {};
    return GpuDeviceIdentity { bytes.release_value() };
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::GpuDeviceIdentity const& identity)
{
    return encoder.encode(identity.bytes);
}

template<>
ErrorOr<Gfx::GpuDeviceIdentity> decode(Decoder& decoder)
{
    auto bytes = TRY(decoder.decode<ByteBuffer>());
    if (bytes.is_empty() || bytes.size() > Gfx::GpuDeviceIdentity::MAXIMUM_BYTE_COUNT)
        return Error::from_string_literal("GPU device identity has an unusable length");
    return Gfx::GpuDeviceIdentity { move(bytes) };
}

}
