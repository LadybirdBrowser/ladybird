/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/Vector.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Painting/ScrollState.h>

namespace IPC {

// The dense in-process vector spans the whole spatial node index space, but only scroll and
// sticky node indices can hold non-zero offsets, so the wire format is sparse (index, offset)
// pairs; holes decode back to zero offsets.
template<>
ErrorOr<void> encode(Encoder& encoder, Web::Painting::ScrollStateSnapshot const& snapshot)
{
    auto device_offsets = snapshot.device_offsets();
    u64 non_zero_offset_count = 0;
    for (auto const& offset : device_offsets) {
        if (!offset.is_zero())
            ++non_zero_offset_count;
    }
    TRY(encoder.encode(non_zero_offset_count));
    for (size_t index = 0; index < device_offsets.size(); ++index) {
        if (device_offsets[index].is_zero())
            continue;
        TRY(encoder.encode(static_cast<u64>(index)));
        TRY(encoder.encode(device_offsets[index]));
    }
    return {};
}

template<>
ErrorOr<Web::Painting::ScrollStateSnapshot> decode(Decoder& decoder)
{
    auto pair_count = TRY(decoder.decode<u64>());
    Web::Painting::ScrollStateSnapshot snapshot;
    for (u64 i = 0; i < pair_count; ++i) {
        auto index = TRY(decoder.decode<u64>());
        auto offset = TRY(decoder.decode<Gfx::FloatPoint>());
        if (index >= NumericLimits<u32>::max())
            return Error::from_string_literal("IPC decode: ScrollStateSnapshot index out of range");
        snapshot.set_device_offset_for_index(Web::Painting::SpatialNodeIndex { static_cast<u32>(index) }, offset);
    }
    return snapshot;
}

}
