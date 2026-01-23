/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibThreading/Thread.h>
#include <LibWeb/HTML/RenderingThread.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>

namespace Web::HTML {

struct BackingStoreState {
    RefPtr<Gfx::PaintingSurface> front_store;
    RefPtr<Gfx::PaintingSurface> back_store;
    i32 front_bitmap_id { -1 };
    i32 back_bitmap_id { -1 };

    void swap()
    {
        AK::swap(front_store, back_store);
        AK::swap(front_bitmap_id, back_bitmap_id);
    }

    bool is_valid() const { return front_store && back_store; }
};

struct UpdateDisplayListCommand {
    NonnullRefPtr<Painting::DisplayList> display_list;
    Painting::ScrollStateSnapshotByDisplayList scroll_state_snapshot;
};

struct UpdateBackingStoresCommand {
    RefPtr<Gfx::PaintingSurface> front_store;
    RefPtr<Gfx::PaintingSurface> back_store;
    i32 front_bitmap_id;
    i32 back_bitmap_id;
};

struct ScreenshotCommand {
    NonnullRefPtr<Gfx::PaintingSurface> target_surface;
    Function<void()> callback;
};

using CompositorCommand = Variant<UpdateDisplayListCommand, UpdateBackingStoresCommand, ScreenshotCommand>;

class RenderingThread::ThreadData final : public AtomicRefCounted<ThreadData> {
public:
    ThreadData(Core::EventLoop& main_thread_event_loop, RenderingThread::PresentationCallback presentation_callback)
        : m_main_thread_event_loop(main_thread_event_loop)
        , m_presentation_callback(move(presentation_callback))
    {
    }

    ~ThreadData() = default;

    void set_skia_player(OwnPtr<Painting::DisplayListPlayerSkia>&& player)
    {
        m_skia_player = move(player);
    }

    bool has_skia_player() const { return m_skia_player != nullptr; }

    void exit()
    {
        Threading::MutexLocker const locker { m_mutex };
        m_exit = true;
        m_command_ready.signal();
        m_ready_to_paint.signal();
    }

    void enqueue_command(CompositorCommand&& command)
    {
        Threading::MutexLocker const locker { m_mutex };
        m_command_queue.enqueue(move(command));
        m_command_ready.signal();
    }

    void set_needs_present(Gfx::IntRect viewport_rect)
    {
        Threading::MutexLocker const locker { m_mutex };
        m_needs_present = true;
        m_pending_viewport_rect = viewport_rect;
        m_command_ready.signal();
    }

    void compositor_loop()
    {
        while (true) {
            {
                Threading::MutexLocker const locker { m_mutex };
                while (m_command_queue.is_empty() && !m_needs_present && !m_exit) {
                    m_command_ready.wait();
                }
                if (m_exit)
                    break;
            }

            while (true) {
                auto command = [this]() -> Optional<CompositorCommand> {
                    Threading::MutexLocker const locker { m_mutex };
                    if (m_command_queue.is_empty())
                        return {};
                    return m_command_queue.dequeue();
                }();

                if (!command.has_value())
                    break;

                command->visit(
                    [this](UpdateDisplayListCommand& cmd) {
                        m_cached_display_list = move(cmd.display_list);
                        m_cached_scroll_state_snapshot = move(cmd.scroll_state_snapshot);
                    },
                    [this](UpdateBackingStoresCommand& cmd) {
                        m_backing_stores.front_store = move(cmd.front_store);
                        m_backing_stores.back_store = move(cmd.back_store);
                        m_backing_stores.front_bitmap_id = cmd.front_bitmap_id;
                        m_backing_stores.back_bitmap_id = cmd.back_bitmap_id;
                    },
                    [this](ScreenshotCommand& cmd) {
                        if (!m_cached_display_list)
                            return;
                        m_skia_player->execute(*m_cached_display_list, Painting::ScrollStateSnapshotByDisplayList(m_cached_scroll_state_snapshot), *cmd.target_surface);
                        if (cmd.callback) {
                            invoke_on_main_thread([callback = move(cmd.callback)]() mutable {
                                callback();
                            });
                        }
                    });

                if (m_exit)
                    break;
            }

            if (m_exit)
                break;

            bool should_present = false;
            Gfx::IntRect viewport_rect;
            {
                Threading::MutexLocker const locker { m_mutex };
                if (m_needs_present) {
                    should_present = true;
                    viewport_rect = m_pending_viewport_rect;
                    m_needs_present = false;
                }
            }

            if (should_present) {
                // Block if we already have a frame queued (back pressure)
                {
                    Threading::MutexLocker const locker { m_mutex };
                    while (m_queued_rasterization_tasks > 1 && !m_exit) {
                        m_ready_to_paint.wait();
                    }
                    if (m_exit)
                        break;
                }

                if (m_cached_display_list && m_backing_stores.is_valid()) {
                    m_skia_player->execute(*m_cached_display_list, Painting::ScrollStateSnapshotByDisplayList(m_cached_scroll_state_snapshot), *m_backing_stores.back_store);
                    i32 rendered_bitmap_id = m_backing_stores.back_bitmap_id;
                    m_backing_stores.swap();

                    m_queued_rasterization_tasks++;

                    invoke_on_main_thread([this, viewport_rect, rendered_bitmap_id]() {
                        m_presentation_callback(viewport_rect, rendered_bitmap_id);
                    });
                }
            }
        }
    }

private:
    template<typename Invokee>
    void invoke_on_main_thread(Invokee invokee)
    {
        if (m_exit)
            return;
        m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr(*this), invokee = move(invokee)]() mutable {
            invokee();
        });
    }

    Core::EventLoop& m_main_thread_event_loop;
    RenderingThread::PresentationCallback m_presentation_callback;

    mutable Threading::Mutex m_mutex;
    mutable Threading::ConditionVariable m_command_ready { m_mutex };
    Atomic<bool> m_exit { false };

    Queue<CompositorCommand> m_command_queue;

    OwnPtr<Painting::DisplayListPlayerSkia> m_skia_player;
    RefPtr<Painting::DisplayList> m_cached_display_list;
    Painting::ScrollStateSnapshotByDisplayList m_cached_scroll_state_snapshot;
    BackingStoreState m_backing_stores;

    Atomic<i32> m_queued_rasterization_tasks { 0 };
    mutable Threading::ConditionVariable m_ready_to_paint { m_mutex };

    bool m_needs_present { false };
    Gfx::IntRect m_pending_viewport_rect;

public:
    void decrement_queued_tasks()
    {
        Threading::MutexLocker const locker { m_mutex };
        VERIFY(m_queued_rasterization_tasks >= 1 && m_queued_rasterization_tasks <= 2);
        m_queued_rasterization_tasks--;
        m_ready_to_paint.signal();
    }
};

RenderingThread::RenderingThread(PresentationCallback presentation_callback)
    : m_thread_data(adopt_ref(*new ThreadData(Core::EventLoop::current(), move(presentation_callback))))
    , m_main_thread_exit_promise(Core::Promise<NonnullRefPtr<Core::EventReceiver>>::construct())
{
    // FIXME: Come up with a better "event loop exited" notification mechanism.
    m_main_thread_exit_promise->on_rejection = [thread_data = m_thread_data](Error const&) -> void {
        thread_data->exit();
    };
    Core::EventLoop::current().add_job(m_main_thread_exit_promise);
}

RenderingThread::~RenderingThread()
{
    // Note: Promise rejection is expected to signal the thread to exit.
    m_main_thread_exit_promise->reject(Error::from_errno(ECANCELED));
    if (m_thread) {
        (void)m_thread->join();
    }
}

void RenderingThread::start(DisplayListPlayerType)
{
    VERIFY(m_thread_data->has_skia_player());
    m_thread = Threading::Thread::construct("Renderer"sv, [thread_data = m_thread_data] {
        thread_data->compositor_loop();
        return static_cast<intptr_t>(0);
    });
    m_thread->start();
}

void RenderingThread::set_skia_player(OwnPtr<Painting::DisplayListPlayerSkia>&& player)
{
    m_thread_data->set_skia_player(move(player));
}

void RenderingThread::update_display_list(NonnullRefPtr<Painting::DisplayList> display_list, Painting::ScrollStateSnapshotByDisplayList&& scroll_state_snapshot)
{
    m_thread_data->enqueue_command(UpdateDisplayListCommand { move(display_list), move(scroll_state_snapshot) });
}

void RenderingThread::update_backing_stores(RefPtr<Gfx::PaintingSurface> front, RefPtr<Gfx::PaintingSurface> back, i32 front_id, i32 back_id)
{
    m_thread_data->enqueue_command(UpdateBackingStoresCommand { move(front), move(back), front_id, back_id });
}

void RenderingThread::present_frame(Gfx::IntRect viewport_rect)
{
    m_thread_data->set_needs_present(viewport_rect);
}

void RenderingThread::request_screenshot(NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    m_thread_data->enqueue_command(ScreenshotCommand { move(target_surface), move(callback) });
}

void RenderingThread::ready_to_paint()
{
    m_thread_data->decrement_queued_tasks();
}

}
