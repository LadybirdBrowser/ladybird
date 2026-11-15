/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/Math.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCompositing/DisplayList/RegisteredTypefaces.h>
#include <LibCompositing/RustFFI.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>

namespace Compositing {

struct DisplayListStoredImageFrameResource {
    AK_ALLOC_WITH_KMALLOC;

    explicit DisplayListStoredImageFrameResource(Gfx::DecodedImageFrame frame)
        : frame(move(frame))
    {
    }

    Gfx::DecodedImageFrame frame;
    // Classifying costs a walk over the pixels and the answer never changes for a given frame — so it's worked out on
    // first use, and kept.
    mutable Optional<bool> force_dark_should_filter;
};

bool DisplayListResourceSet::is_empty() const
{
    return fonts.is_empty()
        && image_frames.is_empty();
}

void DisplayListResourceSet::include(DisplayListResourceSet const& other)
{
    for (auto id : other.fonts)
        fonts.set(id, AK::HashSetExistingEntryBehavior::Keep);
    for (auto id : other.image_frames)
        image_frames.set(id, AK::HashSetExistingEntryBehavior::Keep);
}

DisplayListResourceStorage::DisplayListResourceStorage() = default;
DisplayListResourceStorage::DisplayListResourceStorage(DisplayListResourceStorage&&) = default;
DisplayListResourceStorage& DisplayListResourceStorage::operator=(DisplayListResourceStorage&&) = default;
DisplayListResourceStorage::~DisplayListResourceStorage() = default;

DisplayListFontDescription DisplayListFontDescription::of(Gfx::Font const& font)
{
    return {
        .id = FontResourceId { font.id() },
        .typeface_id = font.typeface().id(),
        .point_size = font.point_size(),
        .variation_settings = font.variation_settings(),
        .synthesis = font.synthesis(),
        .is_invisible = font.is_invisible(),
    };
}

FontResourceId DisplayListResourceStorage::add_font(Gfx::Font const& font)
{
    auto id = font.id();
    m_fonts.ensure(id, [&]() -> NonnullRefPtr<Gfx::Font const> { return font; });
    return { id };
}

// Coarse by design: the verdict only must tell line art from photos; a large image shouldn't pay per-pixel to be asked.
static constexpr size_t max_sampled_pixels = 1000;

static bool classify_image_frame_for_force_dark(Gfx::DecodedImageFrame const& frame)
{
    auto bitmap = frame.bitmap_sharing_pixels();
    auto size = bitmap->size();
    if (size.is_empty())
        return false;

    auto total = static_cast<double>(size.width()) * static_cast<double>(size.height());
    auto step = max(1, static_cast<int>(AK::ceil(AK::sqrt(total / static_cast<double>(max_sampled_pixels)))));

    Vector<u32> opaque_samples;
    size_t transparent_count = 0;
    size_t sampled_count = 0;
    for (int y = 0; y < size.height(); y += step) {
        for (int x = 0; x < size.width(); x += step) {
            auto color = bitmap->get_pixel(x, y);
            sampled_count++;
            // A pixel this sheer says something about the image's shape rather than its palette — so it's counted
            // toward transparency, but kept out of the palette samples.
            if (color.alpha() < 128) {
                transparent_count++;
                continue;
            }
            opaque_samples.append(color.value());
        }
    }
    if (sampled_count == 0)
        return false;

    auto transparency_ratio = static_cast<float>(transparent_count) / static_cast<float>(sampled_count);
    return Compositing::RustFFI::ladybird_web_force_dark_should_filter_image(
        opaque_samples.data(), opaque_samples.size(), transparency_ratio);
}

bool DisplayListResourceStorage::image_frame_should_force_dark(ImageFrameResourceId id) const
{
    auto stored = m_image_frames.get(id.value());
    if (!stored.has_value())
        return false;
    auto const& resource = *stored.value();
    if (!resource.force_dark_should_filter.has_value())
        resource.force_dark_should_filter = classify_image_frame_for_force_dark(resource.frame);
    return resource.force_dark_should_filter.value();
}

ImageFrameResourceId DisplayListResourceStorage::add_image_frame(Gfx::DecodedImageFrame const& frame)
{
    auto id = frame.id();
    m_image_frames.ensure(id, [&] { return make<DisplayListStoredImageFrameResource>(frame); });
    return { id };
}

void DisplayListResourceStorage::set_font(FontResourceId id, NonnullRefPtr<Gfx::Font const> font)
{
    m_fonts.set(id.value(), move(font));
}

void DisplayListResourceStorage::set_image_frame(ImageFrameResourceId id, Gfx::DecodedImageFrame frame)
{
    m_image_frames.set(id.value(), make<DisplayListStoredImageFrameResource>(move(frame)));
}

void DisplayListResourceStorage::replace_image_frame_pixels(ImageFrameResourceId id, Gfx::DecodedImageFrame frame)
{
    auto stored = m_image_frames.get(id.value());
    VERIFY(stored.has_value());
    // NB: Whether force-dark filters the image stays as it was, so that it does not flip between the frames of one
    //     image.
    stored.value()->frame = move(frame);
    m_image_frame_pixel_generations.set(id.value(), m_next_image_frame_pixel_generation++);
}

Gfx::DecodedImageFrame const& DisplayListResourceStorage::image_frame(ImageFrameResourceId id) const
{
    return m_image_frames.get(id.value()).value()->frame;
}

DisplayListResourceSet DisplayListResourceStorage::apply_transaction(DisplayListResourceTransaction&& transaction)
{
    DisplayListResourceSet removed_resources;
    for (auto& frame : transaction.image_frames) {
        if (m_image_frames.contains(frame.id.value()))
            removed_resources.image_frames.set(frame.id);
        set_image_frame(frame.id, move(frame.frame));
    }

    for (auto id : transaction.image_frame_ids_to_remove) {
        m_image_frame_pixel_generations.remove(id.value());
        if (m_image_frames.remove(id.value()))
            removed_resources.image_frames.set(id);
    }
    return removed_resources;
}

bool DisplayListResourceStorage::add_described_fonts_made_of_registered_typefaces(ReadonlySpan<DisplayListFontDescription> descriptions, RegisteredTypefaces const& registered_typefaces)
{
    bool every_typeface_is_registered = true;
    for (auto const& description : descriptions) {
        // NB: Font IDs are immutable. Keep the typeface that Skia's glyph caches were built for.
        if (has_font(description.id))
            continue;
        auto font = registered_typefaces.font_described_by(description);
        if (!font) {
            every_typeface_is_registered = false;
            continue;
        }
        set_font(description.id, font.release_nonnull());
    }
    return every_typeface_is_registered;
}

DisplayListResourceSet DisplayListResourceStorage::retain_only_fonts(ReadonlySpan<u64> font_ids)
{
    HashTable<u64> retained_font_ids;
    for (auto id : font_ids)
        retained_font_ids.set(id);
    DisplayListResourceSet removed_resources;
    m_fonts.remove_all_matching([&](auto id, auto const&) {
        if (retained_font_ids.contains(id))
            return false;
        removed_resources.fonts.set(FontResourceId { id });
        return true;
    });
    return removed_resources;
}

}
