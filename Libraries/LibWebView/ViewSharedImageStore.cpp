/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageInstance.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibWebView/ViewSharedImageStore.h>

namespace WebView {

struct ViewSharedImageStore::SharedBitmap {
    i32 id { -1 };
    Web::DevicePixelSize last_painted_size;
    OwnPtr<Gfx::SharedImageInstance> local_backing;
    RefPtr<Gfx::PaintingSurface> readback_surface;
};

ViewSharedImageStore::ViewSharedImageStore()
    : m_front_bitmap(make<SharedBitmap>())
    , m_back_bitmap(make<SharedBitmap>())
{
}

ViewSharedImageStore::~ViewSharedImageStore() = default;

void ViewSharedImageStore::reset()
{
    m_front_bitmap = make<SharedBitmap>();
    m_back_bitmap = make<SharedBitmap>();
    m_has_usable_bitmap = false;
    m_backup_bitmap = nullptr;
    m_backup_bitmap_size = {};
}

void ViewSharedImageStore::clear_backup_bitmap()
{
    m_backup_bitmap = nullptr;
    m_backup_bitmap_size = {};
}

void ViewSharedImageStore::did_paint(i32 bitmap_id, Gfx::IntSize size, Function<void()> on_ready_to_paint)
{
    if (m_back_bitmap->id == bitmap_id) {
        m_has_usable_bitmap = true;
        m_back_bitmap->last_painted_size = size.to_type<Web::DevicePixels>();
        swap(m_back_bitmap, m_front_bitmap);

        Gfx::Bitmap* bitmap = nullptr;
        if (m_front_bitmap->local_backing)
            bitmap = m_front_bitmap->local_backing->bitmap().ptr();

        auto painting_surface = m_front_bitmap->readback_surface;
        if (painting_surface && bitmap)
            painting_surface->read_into_bitmap(*bitmap);

        if (bitmap)
            clear_backup_bitmap();

        if (on_ready_to_paint)
            on_ready_to_paint();
    }
}

void ViewSharedImageStore::did_allocate_backing_stores(i32 front_bitmap_id, Gfx::SharedImagePayload front_backing_store, i32 back_bitmap_id, Gfx::SharedImagePayload back_backing_store)
{
    if (m_has_usable_bitmap) {
        RefPtr<Gfx::Bitmap> bitmap;
        if (m_front_bitmap->local_backing)
            bitmap = m_front_bitmap->local_backing->bitmap();
        if (bitmap)
            m_backup_bitmap = move(bitmap);
        m_backup_bitmap_size = m_front_bitmap->last_painted_size;
    }

    m_has_usable_bitmap = false;
    m_front_bitmap->id = front_bitmap_id;
    m_back_bitmap->id = back_bitmap_id;

    auto update_bitmap = [](SharedBitmap& target, Gfx::SharedImagePayload backing_store) {
        target.local_backing.clear();
        target.readback_surface = nullptr;

#ifdef USE_VULKAN_DMABUF_IMAGES
        auto skia_backend_context = Gfx::SkiaBackendContext::the();
        Gfx::VulkanContext const* vulkan_context = skia_backend_context ? &skia_backend_context->vulkan_context() : nullptr;
        auto local_backing = Gfx::SharedImageInstance::import_from_payload(move(backing_store), vulkan_context);
#else
        auto local_backing = Gfx::SharedImageInstance::import_from_payload(move(backing_store));
#endif

        if (local_backing.is_error()) {
            warnln("ViewSharedImageStore: failed to import shared image payload: {}", local_backing.error());
            return;
        }

        target.local_backing = make<Gfx::SharedImageInstance>(local_backing.release_value());

#ifdef USE_VULKAN_DMABUF_IMAGES
        if (skia_backend_context && target.local_backing->vulkan_image())
            target.readback_surface = Gfx::PaintingSurface::create_from_image_instance(*target.local_backing, *skia_backend_context, Gfx::PaintingSurface::Origin::TopLeft);
#endif
    };

    update_bitmap(*m_front_bitmap, move(front_backing_store));
    update_bitmap(*m_back_bitmap, move(back_backing_store));
}

Gfx::Bitmap const* ViewSharedImageStore::visible_bitmap() const
{
    if (m_has_usable_bitmap && m_front_bitmap->local_backing)
        return m_front_bitmap->local_backing->bitmap().ptr();
    return m_backup_bitmap.ptr();
}

Gfx::IntSize ViewSharedImageStore::visible_bitmap_size() const
{
    if (m_has_usable_bitmap && m_front_bitmap->local_backing)
        return m_front_bitmap->last_painted_size.to_type<int>();
    return m_backup_bitmap_size.to_type<int>();
}

void* ViewSharedImageStore::visible_bitmap_iosurface_ref() const
{
#ifdef AK_OS_MACOS
    if (m_has_usable_bitmap && m_front_bitmap->local_backing)
        return m_front_bitmap->local_backing->iosurface_handle().core_foundation_pointer();
#endif
    return nullptr;
}

}
