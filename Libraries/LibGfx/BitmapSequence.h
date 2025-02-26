/*
 * Copyright (c) 2024, Zachary Huang <zack466@gmail.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Size.h>
#include <LibIPC/Forward.h>

namespace Gfx {

struct BitmapSequence {
    Vector<RefPtr<Gfx::Bitmap>> bitmaps;
};

// a struct to temporarily store bitmap fields before the buffer data is decoded
struct BitmapMetadata {
    Gfx::BitmapFormat format;
    Gfx::AlphaType alpha_type;
    Gfx::IntSize size;
    size_t size_in_bytes;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::BitmapMetadata const&);

template<>
ErrorOr<Gfx::BitmapMetadata> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::BitmapSequence const&);

template<>
ErrorOr<Gfx::BitmapSequence> decode(Decoder&);

}
