/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
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

struct COMPOSITING_API DisplayListResourceSet {
    bool is_empty() const;
    void include(DisplayListResourceSet const&);

    HashTable<FontResourceId> fonts;
    HashTable<ImageFrameResourceId> image_frames;
};

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

struct DisplayListStoredImageFrameResource;

// A display list recorded in this process, which goes with the visual context tree it was recorded against.
struct DisplayListResourceTransaction {
    Vector<DisplayListImageFrameResource> image_frames;

    Vector<ImageFrameResourceId> image_frame_ids_to_remove;
};

class COMPOSITING_API DisplayListResourceStorage {
    AK_MAKE_NONCOPYABLE(DisplayListResourceStorage);

public:
    DisplayListResourceStorage();
    DisplayListResourceStorage(DisplayListResourceStorage&&);
    DisplayListResourceStorage& operator=(DisplayListResourceStorage&&);
    ~DisplayListResourceStorage();

    FontResourceId add_font(Gfx::Font const&);
    ImageFrameResourceId add_image_frame(Gfx::DecodedImageFrame const&);
    void set_font(FontResourceId, NonnullRefPtr<Gfx::Font const>);
    [[nodiscard]] bool add_described_fonts_made_of_registered_typefaces(ReadonlySpan<DisplayListFontDescription>, RegisteredTypefaces const&);
    // Returns the fonts that were removed.
    DisplayListResourceSet retain_only_fonts(ReadonlySpan<u64> font_ids);
    // Returns the resources that the transaction removed or replaced, so whatever was made from them can be dropped.
    DisplayListResourceSet apply_transaction(DisplayListResourceTransaction&&);
    // Shows other pixels under an id the display lists keep drawing, as an animated image moves to its next frame.
    void replace_image_frame_pixels(ImageFrameResourceId, Gfx::DecodedImageFrame);
    // A stamp for each image frame that has had its pixels replaced. No two replacements share a stamp, even across the
    // removal of a frame, so an equal stamp always means the same pixels.
    HashMap<u64, u64> const& image_frame_pixel_generations() const { return m_image_frame_pixel_generations; }

    bool has_font(FontResourceId id) const { return m_fonts.contains(id.value()); }
    bool has_image_frame(ImageFrameResourceId id) const { return m_image_frames.contains(id.value()); }
    Gfx::Font const& font(FontResourceId id) const { return *m_fonts.get(id.value()).value(); }
    Gfx::DecodedImageFrame const& image_frame(ImageFrameResourceId) const;
    // Whether force-dark should invert this image, worked out once and cached.
    bool image_frame_should_force_dark(ImageFrameResourceId) const;

private:
    void set_image_frame(ImageFrameResourceId, Gfx::DecodedImageFrame);

    HashMap<u64, NonnullRefPtr<Gfx::Font const>> m_fonts;
    HashMap<u64, NonnullOwnPtr<DisplayListStoredImageFrameResource>> m_image_frames;
    HashMap<u64, u64> m_image_frame_pixel_generations;
    u64 m_next_image_frame_pixel_generation { 1 };
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

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayListResourceTransaction const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayListResourceTransaction> decode(Decoder&);

}
