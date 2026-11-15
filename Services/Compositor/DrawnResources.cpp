/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <Compositor/DrawnResources.h>
#include <LibCompositing/DisplayList/RegisteredTypefaces.h>
#include <LibGfx/Font/Font.h>

namespace Compositor {

template<typename Value>
static Vector<u64> retain_only_the_ids(HashMap<u64, Value>& values_by_id, ReadonlySpan<u64> ids)
{
    HashTable<u64> retained_ids;
    for (auto id : ids)
        retained_ids.set(id);
    Vector<u64> removed_ids;
    values_by_id.remove_all_matching([&](auto id, auto const&) {
        if (retained_ids.contains(id))
            return false;
        removed_ids.append(id);
        return true;
    });
    return removed_ids;
}

bool DrawnFonts::add_described_fonts_made_of_registered_typefaces(ReadonlySpan<Compositing::DisplayListFontDescription> descriptions, Compositing::RegisteredTypefaces const& registered_typefaces)
{
    bool every_typeface_is_registered = true;
    for (auto const& description : descriptions) {
        // NB: Font IDs are immutable. Keep the typeface that Skia's glyph caches were built for.
        if (contains(description.id))
            continue;
        auto font = registered_typefaces.font_described_by(description);
        if (!font) {
            every_typeface_is_registered = false;
            continue;
        }
        m_fonts_by_id.set(description.id.value(), font.release_nonnull());
    }
    return every_typeface_is_registered;
}

Compositing::FontResourceId DrawnFonts::add(Gfx::Font const& font)
{
    auto id = font.id();
    m_fonts_by_id.ensure(id, [&]() -> NonnullRefPtr<Gfx::Font const> { return font; });
    return { id };
}

Vector<u64> DrawnFonts::retain_only(ReadonlySpan<u64> font_ids)
{
    return retain_only_the_ids(m_fonts_by_id, font_ids);
}

bool DrawnImageFrames::take_registered(ReadonlySpan<u64> image_frame_ids, Compositing::RegisteredImageFrames const& registered_image_frames)
{
    Vector<NonnullRefPtr<Compositing::RegisteredImageFrame>> image_frames_to_hold;
    Vector<u64> ids_of_image_frames_to_hold;
    for (auto id : image_frame_ids) {
        if (m_image_frames_by_id.contains(id))
            continue;
        auto image_frame = registered_image_frames.image_frame(Compositing::ImageFrameResourceId { id });
        if (!image_frame)
            return false;
        image_frames_to_hold.append(image_frame.release_nonnull());
        ids_of_image_frames_to_hold.append(id);
    }
    for (size_t i = 0; i < image_frames_to_hold.size(); ++i)
        m_image_frames_by_id.set(ids_of_image_frames_to_hold[i], move(image_frames_to_hold[i]));
    return true;
}

Vector<u64> DrawnImageFrames::retain_only(ReadonlySpan<u64> image_frame_ids)
{
    return retain_only_the_ids(m_image_frames_by_id, image_frame_ids);
}

}
