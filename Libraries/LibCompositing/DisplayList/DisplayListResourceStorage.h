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
    Vector<DisplayListFontResource> fonts;
    Vector<DisplayListImageFrameResource> image_frames;

    Vector<FontResourceId> font_ids_to_remove;
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
    // Returns the resources that the transaction removed or replaced, so whatever was made from them can be dropped.
    DisplayListResourceSet apply_transaction(DisplayListResourceTransaction&&);
    DisplayListResourceTransaction create_transaction(DisplayListResourceSet const& previous, DisplayListResourceSet const& current) const;
    // What the list's commands refer to, with what the display lists they nest, and the trees those were recorded
    // against, refer to.
    // What the visual context tree a list recorded in this process was recorded against refers to.
    // Carries everything a list recorded in this process and its tree refer to, for a receiver that holds none of it
    // yet.
    // Returns the resources that were removed.
    DisplayListResourceSet retain_only(DisplayListResourceSet const&);
    bool has_resources_added_since_last_retain() const { return m_has_resources_added_since_last_retain; }
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

    bool m_has_resources_added_since_last_retain { false };
    HashMap<u64, NonnullRefPtr<Gfx::Font const>> m_fonts;
    HashMap<u64, NonnullOwnPtr<DisplayListStoredImageFrameResource>> m_image_frames;
    HashMap<u64, u64> m_image_frame_pixel_generations;
    u64 m_next_image_frame_pixel_generation { 1 };
};

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayListFontResource const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayListFontResource> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayListImageFrameResource const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayListImageFrameResource> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::DisplayListResourceTransaction const&);
template<>
COMPOSITING_API ErrorOr<Compositing::DisplayListResourceTransaction> decode(Decoder&);

}
