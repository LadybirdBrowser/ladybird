/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/HTML/RenderingThread.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Painting/BackingStore.h>

namespace Web::HTML {

RenderingThread::RenderingThread()
    : m_main_thread_event_loop(Core::EventLoop::current())
{
}

RenderingThread::~RenderingThread()
{
    m_exit = true;
    m_rendering_task_ready_wake_condition.signal();
    (void)m_thread->join();
}

void RenderingThread::start(DisplayListPlayerType display_list_player_type)
{
    m_display_list_player_type = display_list_player_type;
    VERIFY(m_skia_player);
    m_thread = Threading::Thread::construct([this] {
        rendering_thread_loop();
        return static_cast<intptr_t>(0);
    });
    m_thread->start();
}

void RenderingThread::rendering_thread_loop()
{
    while (true) {
        auto task = [this]() -> Optional<Task> {
            Threading::MutexLocker const locker { m_rendering_task_mutex };
            if (m_needs_to_clear_bitmap_to_surface_cache) {
                m_bitmap_to_surface.clear();
                m_needs_to_clear_bitmap_to_surface_cache = false;
            }
            while (m_rendering_tasks.is_empty() && !m_exit) {
                m_rendering_task_ready_wake_condition.wait();
            }
            if (m_exit)
                return {};
            return m_rendering_tasks.dequeue();
        }();

        if (!task.has_value()) {
            VERIFY(m_exit);
            break;
        }

        auto painting_surface = painting_surface_for_backing_store(task->backing_store);
        m_skia_player->execute(*task->display_list, painting_surface);
        m_main_thread_event_loop.deferred_invoke([callback = move(task->callback)] {
            callback();
        });
    }
}

void RenderingThread::enqueue_rendering_task(NonnullRefPtr<Painting::DisplayList> display_list, NonnullRefPtr<Painting::BackingStore> backing_store, Function<void()>&& callback)
{
    Threading::MutexLocker const locker { m_rendering_task_mutex };
    m_rendering_tasks.enqueue(Task { move(display_list), move(backing_store), move(callback) });
    m_rendering_task_ready_wake_condition.signal();
}

NonnullRefPtr<Gfx::PaintingSurface> RenderingThread::painting_surface_for_backing_store(Painting::BackingStore& backing_store)
{
    auto& bitmap = backing_store.bitmap();
    auto cached_surface = m_bitmap_to_surface.find(&bitmap);
    if (cached_surface != m_bitmap_to_surface.end())
        return cached_surface->value;

    RefPtr<Gfx::PaintingSurface> new_surface;
    if (m_display_list_player_type == DisplayListPlayerType::SkiaGPUIfAvailable && m_skia_backend_context) {
#ifdef USE_VULKAN
        // Vulkan: Try to create an accelerated surface.
        new_surface = Gfx::PaintingSurface::create_with_size(m_skia_backend_context, backing_store.size(), Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
        new_surface->on_flush = [backing_store = static_cast<NonnullRefPtr<Painting::BackingStore>>(backing_store)](auto& surface) { surface.read_into_bitmap(backing_store->bitmap()); };
#endif
#ifdef AK_OS_MACOS
        // macOS: Wrap an IOSurface if available.
        if (is<Painting::IOSurfaceBackingStore>(backing_store)) {
            auto& iosurface_backing_store = static_cast<Painting::IOSurfaceBackingStore&>(backing_store);
            new_surface = Gfx::PaintingSurface::wrap_iosurface(iosurface_backing_store.iosurface_handle(), *m_skia_backend_context);
        }
#endif
    }

    // CPU and fallback: wrap the backing store bitmap directly.
    if (!new_surface)
        new_surface = Gfx::PaintingSurface::wrap_bitmap(bitmap);

    m_bitmap_to_surface.set(&bitmap, *new_surface);
    return *new_surface;
}

void RenderingThread::clear_bitmap_to_surface_cache()
{
    Threading::MutexLocker const locker { m_rendering_task_mutex };
    m_needs_to_clear_bitmap_to_surface_cache = true;
}

}
