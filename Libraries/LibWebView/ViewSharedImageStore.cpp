/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibWebView/ViewSharedImageStore.h>

namespace WebView {

ViewSharedImageStore::ViewSharedImageStore() = default;

ViewSharedImageStore::~ViewSharedImageStore() = default;

void ViewSharedImageStore::reset()
{
    m_front_bitmap = {};
    m_back_bitmap = {};
    m_has_usable_bitmap = false;
    m_backup_shared_image_buffer = nullptr;
    m_backup_bitmap_size = {};
}

void ViewSharedImageStore::clear_backup_bitmap()
{
    m_backup_shared_image_buffer = nullptr;
    m_backup_bitmap_size = {};
}

void ViewSharedImageStore::did_paint(i32 bitmap_id, Gfx::IntSize size, Function<void()> on_ready_to_paint)
{
    if (m_back_bitmap.id == bitmap_id) {
        m_has_usable_bitmap = true;
        m_back_bitmap.last_painted_size = size.to_type<Web::DevicePixels>();
        swap(m_back_bitmap, m_front_bitmap);
        clear_backup_bitmap();

        if (on_ready_to_paint)
            on_ready_to_paint();
    }
}

void ViewSharedImageStore::did_allocate_backing_stores(i32 front_bitmap_id, Gfx::SharedImage front_backing_store, i32 back_bitmap_id, Gfx::SharedImage back_backing_store)
{
    if (m_has_usable_bitmap) {
        // NOTE: We keep the outgoing front bitmap as a backup so we have something to paint until we get a new one.
        m_backup_shared_image_buffer = move(m_front_bitmap.shared_image_buffer);
        m_backup_bitmap_size = m_front_bitmap.last_painted_size;
    }

    m_has_usable_bitmap = false;
    m_front_bitmap.id = front_bitmap_id;
    m_back_bitmap.id = back_bitmap_id;
    m_front_bitmap.shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::import_from_payload(move(front_backing_store)).release_value_but_fixme_should_propagate_errors());
    m_back_bitmap.shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::import_from_payload(move(back_backing_store)).release_value_but_fixme_should_propagate_errors());
}

Gfx::SharedImageBuffer const* ViewSharedImageStore::visible_shared_image_buffer() const
{
    if (m_has_usable_bitmap)
        return m_front_bitmap.shared_image_buffer.ptr();
    return m_backup_shared_image_buffer.ptr();
}

Gfx::Bitmap const* ViewSharedImageStore::visible_bitmap() const
{
    auto const* shared_image_buffer = visible_shared_image_buffer();
    if (!shared_image_buffer)
        return nullptr;
    return shared_image_buffer->bitmap().ptr();
}

Gfx::IntSize ViewSharedImageStore::visible_bitmap_size() const
{
    if (m_has_usable_bitmap)
        return m_front_bitmap.last_painted_size.to_type<int>();
    return m_backup_bitmap_size.to_type<int>();
}

}