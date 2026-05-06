/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGfx/Size.h>
#include <LibIPC/Forward.h>

namespace Gfx {

enum class BitmapFormat : u8 {
    BGRA8888 = 1,
    BGRx8888,
    RGBA8888,
    RGBx8888,
    RGBAF16,
    Gray8,
    Alpha8,
    RGB565,
    RGBA5551,
    RGBA4444,
    RGB888,
};

enum class BitmapColorSpace : u8 {
    SRGB = 1,
    Linear,
};

enum class BitmapAlpha : u8 {
    Premultiplied = 1,
    Unpremultiplied,
    Opaque,
};

enum class BitmapOrigin : u8 {
    TopLeft = 1,
    BottomLeft,
};

struct BitmapInfo {
    IntSize size;
    u32 row_bytes { 0 };
    u16 mip_level_count { 1 };
    u16 sample_count { 1 };
    u64 tiling_modifier { 0 };
    BitmapFormat pixel_format { BitmapFormat::BGRA8888 };
    BitmapColorSpace color_space { BitmapColorSpace::SRGB };
    BitmapAlpha alpha_type { BitmapAlpha::Premultiplied };
    BitmapOrigin origin { BitmapOrigin::TopLeft };
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::BitmapFormat const&);

template<>
ErrorOr<Gfx::BitmapFormat> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::BitmapColorSpace const&);

template<>
ErrorOr<Gfx::BitmapColorSpace> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::BitmapAlpha const&);

template<>
ErrorOr<Gfx::BitmapAlpha> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::BitmapOrigin const&);

template<>
ErrorOr<Gfx::BitmapOrigin> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Gfx::BitmapInfo const&);

template<>
ErrorOr<Gfx::BitmapInfo> decode(Decoder&);

}
