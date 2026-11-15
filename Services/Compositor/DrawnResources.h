/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibCompositing/DisplayList/DisplayListResources.h>
#include <LibCompositing/DisplayList/RegisteredImageFrames.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/Forward.h>

namespace Compositor {

// The fonts that a display list, the overlays, a batch of a canvas 2D stream or a staged rasterization draws, each
// under the id that its commands name it by.
class DrawnFonts {
public:
    [[nodiscard]] bool add_described_fonts_made_of_registered_typefaces(ReadonlySpan<Compositing::DisplayListFontDescription>, Compositing::RegisteredTypefaces const&);
    Compositing::FontResourceId add(Gfx::Font const&);

    bool contains(Compositing::FontResourceId id) const { return m_fonts_by_id.contains(id.value()); }
    Gfx::Font const& font(Compositing::FontResourceId id) const { return *m_fonts_by_id.get(id.value()).value(); }

    // Returns the ids of the fonts that were removed.
    Vector<u64> retain_only(ReadonlySpan<u64> font_ids);

private:
    HashMap<u64, NonnullRefPtr<Gfx::Font const>> m_fonts_by_id;
};

// The registered image frames that a display list and its visual context tree, or a staged rasterization, draw.
class DrawnImageFrames {
public:
    // Holds the image frame registered under each id that it holds none for, and answers whether it then holds one
    // for every id. It takes none where one of them is missing.
    [[nodiscard]] bool take_registered(ReadonlySpan<u64> image_frame_ids, Compositing::RegisteredImageFrames const&);

    size_t size() const { return m_image_frames_by_id.size(); }
    bool contains(Compositing::ImageFrameResourceId id) const { return m_image_frames_by_id.contains(id.value()); }
    Compositing::RegisteredImageFrame const& image_frame(Compositing::ImageFrameResourceId id) const { return *m_image_frames_by_id.get(id.value()).value(); }
    Compositing::RegisteredImageFrame& image_frame(Compositing::ImageFrameResourceId id) { return *m_image_frames_by_id.get(id.value()).value(); }

    template<typename Callback>
    void for_each(Callback callback) const
    {
        for (auto const& [id, image_frame] : m_image_frames_by_id)
            callback(Compositing::ImageFrameResourceId { id }, *image_frame);
    }

    // Returns the ids of the image frames that were removed.
    Vector<u64> retain_only(ReadonlySpan<u64> image_frame_ids);

private:
    HashMap<u64, NonnullRefPtr<Compositing::RegisteredImageFrame>> m_image_frames_by_id;
};

}
