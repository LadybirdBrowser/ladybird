/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <WebContent/BackingStoreManager.h>
#include <WebContent/PageClient.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#    include <LibCore/MachPort.h>
#    include <LibCore/Platform/MachMessageTypes.h>
#endif

namespace WebContent {

#ifdef AK_OS_MACOS
static Optional<Core::MachPort> s_browser_mach_port;
void BackingStoreManager::set_browser_mach_port(Core::MachPort&& port)
{
    s_browser_mach_port = move(port);
}
#endif

BackingStoreManager::BackingStoreManager(PageClient& page_client)
    : m_page_client(page_client)
{
    m_backing_store_shrink_timer = Core::Timer::create_single_shot(3000, [this] {
        resize_backing_stores_if_needed(WindowResizingInProgress::No);
    });
}

void BackingStoreManager::restart_resize_timer()
{
    m_backing_store_shrink_timer->restart();
}

void BackingStoreManager::reallocate_backing_stores(Gfx::IntSize size)
{
#ifdef AK_OS_MACOS
    if (s_browser_mach_port.has_value()) {
        auto back_iosurface = Core::IOSurfaceHandle::create(size.width(), size.height());
        auto back_iosurface_port = back_iosurface.create_mach_port();

        auto front_iosurface = Core::IOSurfaceHandle::create(size.width(), size.height());
        auto front_iosurface_port = front_iosurface.create_mach_port();

        m_front_bitmap_id = m_next_bitmap_id++;
        m_back_bitmap_id = m_next_bitmap_id++;

        Core::Platform::BackingStoreMetadata metadata;
        metadata.page_id = m_page_client.m_id;
        metadata.front_backing_store_id = m_front_bitmap_id;
        metadata.back_backing_store_id = m_back_bitmap_id;

        Core::Platform::MessageWithBackingStores message;

        message.header.msgh_remote_port = s_browser_mach_port->port();
        message.header.msgh_local_port = MACH_PORT_NULL;
        message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
        message.header.msgh_size = sizeof(message);
        message.header.msgh_id = Core::Platform::BACKING_STORE_IOSURFACES_MESSAGE_ID;

        message.body.msgh_descriptor_count = 2;

        message.front_descriptor.name = front_iosurface_port.release();
        message.front_descriptor.disposition = MACH_MSG_TYPE_MOVE_SEND;
        message.front_descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

        message.back_descriptor.name = back_iosurface_port.release();
        message.back_descriptor.disposition = MACH_MSG_TYPE_MOVE_SEND;
        message.back_descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

        message.metadata = metadata;

        mach_msg_timeout_t const timeout = 100; // milliseconds
        auto const send_result = mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, message.header.msgh_size, 0, MACH_PORT_NULL, timeout, MACH_PORT_NULL);
        if (send_result != KERN_SUCCESS) {
            dbgln("Failed to send message to server: {}", mach_error_string(send_result));
            VERIFY_NOT_REACHED();
        }

        m_front_store = Web::Painting::IOSurfaceBackingStore::create(move(front_iosurface));
        m_back_store = Web::Painting::IOSurfaceBackingStore::create(move(back_iosurface));

        return;
    }
#endif

    m_front_bitmap_id = m_next_bitmap_id++;
    m_back_bitmap_id = m_next_bitmap_id++;

    auto front_bitmap = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size).release_value();
    auto back_bitmap = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size).release_value();

    m_front_store = Web::Painting::BitmapBackingStore::create(front_bitmap);
    m_back_store = Web::Painting::BitmapBackingStore::create(back_bitmap);

    m_page_client.page_did_allocate_backing_stores(m_front_bitmap_id, front_bitmap->to_shareable_bitmap(), m_back_bitmap_id, back_bitmap->to_shareable_bitmap());
}

void BackingStoreManager::resize_backing_stores_if_needed(WindowResizingInProgress window_resize_in_progress)
{
    auto css_pixels_viewpor_rect = m_page_client.page().top_level_traversable()->viewport_rect();
    auto viewport_size = m_page_client.page().css_to_device_rect(css_pixels_viewpor_rect).size();

    if (viewport_size.is_empty())
        return;

    Web::DevicePixelSize minimum_needed_size;
    if (window_resize_in_progress == WindowResizingInProgress::Yes) {
        // Pad the minimum needed size so that we don't have to keep reallocating backing stores while the window is being resized.
        minimum_needed_size = { viewport_size.width() + 256, viewport_size.height() + 256 };
    } else {
        // If we're not in the middle of a resize, we can shrink the backing store size to match the viewport size.
        minimum_needed_size = viewport_size;
        m_front_store.clear();
        m_back_store.clear();
    }

    if (!m_front_store || !m_back_store || !m_front_store->size().contains(minimum_needed_size.to_type<int>())) {
        reallocate_backing_stores(minimum_needed_size.to_type<int>());
    }
}

void BackingStoreManager::swap_back_and_front()
{
    swap(m_front_store, m_back_store);
    swap(m_front_bitmap_id, m_back_bitmap_id);
}

}
