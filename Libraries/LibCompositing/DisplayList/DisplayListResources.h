/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontVariationSettings.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>

namespace Compositing {

struct DisplayListFontResource {
    FontResourceId id;
    NonnullRefPtr<Gfx::Font const> font;
};

struct COMPOSITING_API DisplayListFontDescription {
    static DisplayListFontDescription of(Gfx::Font const&);

    FontResourceId id;
    u64 typeface_id { 0 };
    float point_size { 0 };
    Gfx::FontVariationSettings variation_settings;
    Gfx::FontSynthesis synthesis;
    bool is_invisible { false };
};

struct TypefaceRegistration {
    u64 typeface_id { 0 };
    NonnullRefPtr<Gfx::Typeface const> typeface;
};

struct DisplayListImageFrameResource {
    ImageFrameResourceId id;
    Gfx::DecodedImageFrame frame;
};

// The -webkit-font-smoothing values the recorder writes as a byte, in the order the CSS enum declares them.
enum class FontSmoothing : u8 {
    Auto,
    None,
    Antialiased,
    SubpixelAntialiased,
};

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayListFontDescription const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayListFontDescription> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::TypefaceRegistration const&);
template<>
COMPOSITING_API ErrorOr<Compositing::TypefaceRegistration> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayListImageFrameResource const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayListImageFrameResource> decode(Decoder&);

}
