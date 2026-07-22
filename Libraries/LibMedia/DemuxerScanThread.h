/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/ThreadID.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/MediaStream.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>

namespace Media {

// Runs a demuxer's buffered scan on its own thread, rescanning whenever the stream's available data changes.
template<typename Payload>
class DemuxerScanThread final : public AtomicRefCounted<DemuxerScanThread<Payload>> {
public:
    using ScanFunction = Function<DemuxerScanState(MediaStream&, Payload&)>;

    static NonnullRefPtr<DemuxerScanThread> start(NonnullRefPtr<MediaStream> stream, DemuxerScanState initial_state, Payload payload, ScanFunction scan)
    {
        auto scan_thread = adopt_ref(*new DemuxerScanThread(move(stream), move(initial_state), move(payload), move(scan)));

        scan_thread->m_stream->set_available_ranges_change_observer([scan_thread] {
            Sync::MutexLocker locker { scan_thread->m_mutex };
            scan_thread->m_rescan_requested = true;
            scan_thread->m_condition.broadcast();
        });

        auto thread = Threading::Thread::construct("Buffered Scan"sv, [scan_thread]() -> int {
            scan_thread->run();
            return 0;
        });
        thread->start();
        thread->detach();

        return scan_thread;
    }

    void shutdown()
    {
        m_stream->set_available_ranges_change_observer(nullptr);
        Sync::MutexLocker locker { m_mutex };
        m_exit_requested = true;
        m_condition.broadcast();
    }

    Payload& payload() { return m_payload; }
    Payload const& payload() const { return m_payload; }

    DemuxerScanState const& main_thread_state() const LIFETIME_BOUND
    {
        if (m_handler_thread_id.is_valid())
            VERIFY(m_handler_thread_id.is_current_thread());
        return m_main_thread_state;
    }

    void set_change_handler(Function<void()> handler)
    {
        if (m_handler_thread_id.is_valid())
            VERIFY(m_handler_thread_id.is_current_thread());
        m_handler_thread_id = AK::ThreadID::current();
        m_on_change = move(handler);

        Sync::MutexLocker locker { m_mutex };
        m_handler_event_loop = &Core::EventLoop::current();

        // Deliver any state that was scanned before a home event loop existed.
        m_rescan_requested = true;
        m_condition.broadcast();
    }

private:
    DemuxerScanThread(NonnullRefPtr<MediaStream> stream, DemuxerScanState initial_state, Payload payload, ScanFunction scan)
        : m_stream(move(stream))
        , m_payload(move(payload))
        , m_scan(move(scan))
        , m_main_thread_state(move(initial_state))
    {
    }

    void run()
    {
        while (true) {
            {
                Sync::MutexLocker locker { m_mutex };
                while (!m_rescan_requested && !m_exit_requested)
                    m_condition.wait();
                if (m_exit_requested)
                    return;
                m_rescan_requested = false;
            }

            auto new_state = m_scan(*m_stream, m_payload);

            if (new_state == m_last_dispatched_state)
                continue;

            Core::EventLoop* handler_event_loop;
            {
                Sync::MutexLocker locker { m_mutex };
                handler_event_loop = m_handler_event_loop;
            }
            if (handler_event_loop == nullptr)
                continue;

            m_last_dispatched_state = new_state;
            handler_event_loop->deferred_invoke([self = NonnullRefPtr(*this), new_state = move(new_state)]() mutable {
                self->m_main_thread_state = move(new_state);
                if (self->m_on_change)
                    self->m_on_change();
            });
        }
    }

    NonnullRefPtr<MediaStream> m_stream;
    Payload m_payload;
    ScanFunction m_scan;

    Sync::Mutex m_mutex;
    Sync::ConditionVariable m_condition { m_mutex };
    bool m_rescan_requested { true };
    bool m_exit_requested { false };
    Core::EventLoop* m_handler_event_loop { nullptr };

    DemuxerScanState m_last_dispatched_state;

    AK::ThreadID m_handler_thread_id;
    DemuxerScanState m_main_thread_state;
    Function<void()> m_on_change;
};

}
