/*
 * Copyright (c) 2024-2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Types.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>

namespace Gfx {

enum class ColorFilterType {
    Brightness,
    Contrast,
    Grayscale,
    Invert,
    Opacity,
    Saturate,
    Sepia
};

// An image filter graph in the serialized form painting hands across the FFI, the display list and
// IPC. Graphs are built on the Rust side; this side carries them and reads them into Skia.
class Filter {
public:
    explicit Filter(ByteBuffer serialized_bytes);

    ReadonlyBytes serialized_bytes() const { return m_serialized_bytes; }
    ByteBuffer take_serialized_bytes() && { return move(m_serialized_bytes); }

private:
    ByteBuffer m_serialized_bytes;
};

// Visits the id of every image frame a serialized filter graph draws. Bytes that do not hold a
// filter graph visit nothing.
void for_each_filter_image_frame_id(ReadonlyBytes serialized_filter, Function<void(u64)> const&);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::Filter const&);

template<>
ErrorOr<Gfx::Filter> decode(Decoder&);

}
