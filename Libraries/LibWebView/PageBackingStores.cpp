/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/PageBackingStores.h>

namespace WebView {

Optional<PageBackingStores::Frame> PageBackingStores::displayed_frame() const
{
    if (m_has_usable_front_bitmap)
        return Frame { m_front_bitmap.shared_image_buffer.ptr(), m_front_bitmap.last_painted_size.to_type<int>(), m_front_bitmap.last_painted_device_pixel_ratio, m_front_bitmap.id };
    if (m_has_backup)
        return Frame { m_backup.shared_image_buffer.ptr(), m_backup.last_painted_size.to_type<int>(), m_backup.last_painted_device_pixel_ratio, {} };
    return {};
}

Vector<Gfx::SharedImageBuffer const*> PageBackingStores::shared_image_buffers() const
{
    Vector<Gfx::SharedImageBuffer const*> shared_image_buffers;
    if (m_front_bitmap.shared_image_buffer)
        shared_image_buffers.append(m_front_bitmap.shared_image_buffer.ptr());
    for (auto const& bitmap : m_other_bitmaps) {
        if (bitmap.shared_image_buffer)
            shared_image_buffers.append(bitmap.shared_image_buffer.ptr());
    }
    if (m_backup.shared_image_buffer)
        shared_image_buffers.append(m_backup.shared_image_buffer.ptr());
    return shared_image_buffers;
}

void PageBackingStores::track_installed_backing_stores(ReadonlySpan<i32> bitmap_ids)
{
    VERIFY(!bitmap_ids.is_empty());

    // NOTE: We keep the outgoing front bitmap as a backup so we have something to paint until we get a new one.
    PageBackingStores::keep_front_as_backup();
    m_has_usable_front_bitmap = false;
    m_front_bitmap.id = bitmap_ids[0];
    m_front_bitmap.shared_image_buffer = nullptr;
    m_other_bitmaps.clear();
    track_added_backing_stores(bitmap_ids.slice(1));
}

void PageBackingStores::track_added_backing_stores(ReadonlySpan<i32> bitmap_ids)
{
    m_other_bitmaps.ensure_capacity(m_other_bitmaps.size() + bitmap_ids.size());
    for (auto bitmap_id : bitmap_ids)
        m_other_bitmaps.append({ .id = bitmap_id, .last_painted_size = {}, .shared_image_buffer = nullptr });
}

void PageBackingStores::install(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    VERIFY(bitmap_ids.size() == backing_stores.size());
    track_installed_backing_stores(bitmap_ids);
    m_front_bitmap.shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::import_from_shared_image(move(backing_stores[0])));
    for (size_t i = 1; i < backing_stores.size(); ++i)
        m_other_bitmaps[i - 1].shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::import_from_shared_image(move(backing_stores[i])));
}

void PageBackingStores::add(Vector<i32> bitmap_ids, Vector<Gfx::SharedImage> backing_stores)
{
    VERIFY(bitmap_ids.size() == backing_stores.size());
    auto first_new_bitmap = m_other_bitmaps.size();
    track_added_backing_stores(bitmap_ids);
    for (size_t i = 0; i < backing_stores.size(); ++i)
        m_other_bitmaps[first_new_bitmap + i].shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::import_from_shared_image(move(backing_stores[i])));
}

void PageBackingStores::retire(ReadonlySpan<i32> bitmap_ids)
{
    m_other_bitmaps.remove_all_matching([&](auto const& bitmap) {
        return bitmap_ids.contains_slow(bitmap.id);
    });
}

PageBackingStores::PresentationResult PageBackingStores::present(i32 bitmap_id, Gfx::IntSize size, double device_pixel_ratio)
{
    auto bitmap_index = m_other_bitmaps.find_first_index_if([bitmap_id](auto const& bitmap) { return bitmap_id == bitmap.id; });
    if (!bitmap_index.has_value())
        return { .did_swap = false, .bitmap_id_to_release = bitmap_id };

    auto previous_front_bitmap_id = m_front_bitmap.id;
    m_has_usable_front_bitmap = true;
    m_other_bitmaps[*bitmap_index].last_painted_size = size.to_type<Web::DevicePixels>();
    m_other_bitmaps[*bitmap_index].last_painted_device_pixel_ratio = device_pixel_ratio;
    swap(m_other_bitmaps[*bitmap_index], m_front_bitmap);
    PageBackingStores::clear_backup();
    return { .did_swap = true, .bitmap_id_to_release = previous_front_bitmap_id };
}

void PageBackingStores::keep_front_as_backup()
{
    if (!m_has_usable_front_bitmap)
        return;
    m_backup.shared_image_buffer = move(m_front_bitmap.shared_image_buffer);
    m_backup.last_painted_size = m_front_bitmap.last_painted_size;
    m_backup.last_painted_device_pixel_ratio = m_front_bitmap.last_painted_device_pixel_ratio;
    m_has_backup = true;
}

void PageBackingStores::clear_backup()
{
    m_backup = {};
    m_has_backup = false;
}

void PageBackingStores::clear()
{
    m_front_bitmap = {};
    m_other_bitmaps.clear();
    m_has_usable_front_bitmap = false;
}

}
