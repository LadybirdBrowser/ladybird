/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Filter.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <RustFFI.h>

namespace Gfx {

Filter::Filter(ByteBuffer serialized_bytes)
    : m_serialized_bytes(move(serialized_bytes))
{
}

void for_each_filter_image_frame_id(ReadonlyBytes serialized_filter, Function<void(u64)> const& visit)
{
    FFI::ladybird_gfx_filter_for_each_image_frame_id(
        serialized_filter.data(),
        serialized_filter.size(),
        [](void const* context, u64 image_frame_id) {
            (*static_cast<Function<void(u64)> const*>(context))(image_frame_id);
        },
        &visit);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::Filter const& filter)
{
    return encoder.encode(filter.serialized_bytes());
}

template<>
ErrorOr<Gfx::Filter> decode(Decoder& decoder)
{
    return Gfx::Filter { TRY(decoder.decode<ByteBuffer>()) };
}

}
