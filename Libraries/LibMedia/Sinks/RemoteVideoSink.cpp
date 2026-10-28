/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AtomicRefCounted.h>
#include <AK/ConditionVariable.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/QuickSort.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMedia/Sinks/RemoteVideoSink.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibThreading/Thread.h>

namespace Media {

class RemoteVideoSink::ThreadData : public AtomicRefCounted<RemoteVideoSink::ThreadData> {
public:
    explicit ThreadData(Delegates);
    ~ThreadData();

    ErrorOr<void> connect_input(NonnullRefPtr<VideoProducer> const&);
    void disconnect_input(NonnullRefPtr<VideoProducer> const&);
    void notify_clock_resumed() const;
    void transmit_seek(InvalidateHeldData) const;
    void transmit_time_reader(MediaTimeReader const&) const;
    void seek_upstream(AK::Duration timestamp, InvalidateHeldData, u64 newest_received_lend_id);
    void seek_input_directly(AK::Duration timestamp);
    void start_input();
    void release_frame(u64 lend_id);

    void pump_thread_loop();
    void request_exit();
    void stop();
    bool has_lent_frames() const;

private:
    struct PublishedStatus {
        PipelineStatus status { PipelineStatus::Pending };
        u32 seek_id { 0 };

        bool operator==(PublishedStatus const&) const = default;
    };

    void lend_frame(NonnullRefPtr<VideoFrame> const&, u32 seek_id);
    void publish_status(PublishedStatus);
    bool consumer_may_satisfy_seek_locally(AK::Duration timestamp) const;
    Vector<NonnullRefPtr<VideoFrame>> frames_lent_after_holding(u64 newest_received_lend_id, AK::Duration timestamp) const;
    void wait_on_pump_thread(MutexLocker<Mutex>&);
    void wake_pump_thread();

    Delegates m_delegates;

    mutable Mutex m_mutex;
    ConditionVariable m_wait_condition { m_mutex };
    RefPtr<VideoProducer> m_input;
    bool m_exit_requested { false };
    bool m_pump_thread_exited { false };
    bool m_wake_pending { false };
    u32 m_actual_seek_id { 0 };
    // A seek the input was given directly, whose request from the consumer has yet to arrive.
    Optional<AK::Duration> m_direct_seek_timestamp;
    // Still in the input until it has been lent.
    RefPtr<VideoFrame> m_peeked_frame;
    // Lent again under the next seek's stamp before anything newer is taken from the input.
    Vector<NonnullRefPtr<VideoFrame>> m_frames_to_lend_again;
    Optional<PublishedStatus> m_published_status;

    mutable Mutex m_lent_frames_mutex;
    OrderedHashMap<u64, NonnullRefPtr<VideoFrame>> m_lent_frames;
    u64 m_next_lend_id { 1 };
};

ErrorOr<NonnullRefPtr<RemoteVideoSink>> RemoteVideoSink::create(Delegates delegates)
{
    VERIFY(delegates.send_frame);
    VERIFY(delegates.send_status);
    VERIFY(delegates.notify_clock_resumed);
    VERIFY(delegates.transmit_seek);
    VERIFY(delegates.transmit_time_reader);
    auto thread_data = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) ThreadData(move(delegates))));
    auto sink = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) RemoteVideoSink(thread_data)));
    auto thread = Threading::Thread::construct("Video Pump"sv, [thread_data]() -> intptr_t {
        thread_data->pump_thread_loop();
        return 0;
    });
    thread->start();
    thread->detach();
    return sink;
}

RemoteVideoSink::RemoteVideoSink(NonnullRefPtr<ThreadData> thread_data)
    : m_thread_data(move(thread_data))
{
}

RemoteVideoSink::~RemoteVideoSink()
{
    m_thread_data->request_exit();
}

ErrorOr<void> RemoteVideoSink::connect_input(NonnullRefPtr<VideoProducer> const& producer) { return m_thread_data->connect_input(producer); }
void RemoteVideoSink::disconnect_input(NonnullRefPtr<VideoProducer> const& producer) { m_thread_data->disconnect_input(producer); }

void RemoteVideoSink::seek(AK::Duration timestamp, InvalidateHeldData invalidate_held_data)
{
    if (invalidate_held_data == InvalidateHeldData::No)
        m_thread_data->seek_input_directly(timestamp);
    m_thread_data->transmit_seek(invalidate_held_data);
}
void RemoteVideoSink::notify_clock_resumed() { m_thread_data->notify_clock_resumed(); }
void RemoteVideoSink::seek_upstream(AK::Duration timestamp, InvalidateHeldData invalidate_held_data, u64 newest_received_lend_id) { m_thread_data->seek_upstream(timestamp, invalidate_held_data, newest_received_lend_id); }

void RemoteVideoSink::set_time_reader(MediaTimeReader time_reader)
{
    m_thread_data->transmit_time_reader(time_reader);
}

void RemoteVideoSink::set_state_change_handler(PipelineStateChangeHandler on_state_changed)
{
    m_on_state_changed = move(on_state_changed);
}

void RemoteVideoSink::set_resize_handler(PipelineResizeHandler on_resize)
{
    m_on_resize = move(on_resize);
}

void RemoteVideoSink::report_remote_status(PipelineStatus status)
{
    if (m_on_state_changed)
        m_on_state_changed(status);
}

void RemoteVideoSink::report_remote_resize(Gfx::Size<u32> size)
{
    if (m_on_resize)
        m_on_resize(size);
}

void RemoteVideoSink::start_input() { m_thread_data->start_input(); }
void RemoteVideoSink::release_frame(u64 lend_id) { m_thread_data->release_frame(lend_id); }
void RemoteVideoSink::stop() { m_thread_data->stop(); }
bool RemoteVideoSink::has_lent_frames() const { return m_thread_data->has_lent_frames(); }

RemoteVideoSink::ThreadData::ThreadData(Delegates delegates)
    : m_delegates(move(delegates))
{
}

RemoteVideoSink::ThreadData::~ThreadData() = default;

ErrorOr<void> RemoteVideoSink::ThreadData::connect_input(NonnullRefPtr<VideoProducer> const& producer)
{
    producer->set_wake_handler([this] { wake_pump_thread(); });
    MutexLocker locker { m_mutex };
    VERIFY(m_input == nullptr);
    m_input = producer;
    m_wake_pending = true;
    m_wait_condition.broadcast();
    return {};
}

void RemoteVideoSink::ThreadData::disconnect_input(NonnullRefPtr<VideoProducer> const& producer)
{
    producer->set_wake_handler(nullptr);
    MutexLocker locker { m_mutex };
    VERIFY(m_input == producer.ptr());
    m_input = nullptr;
    m_direct_seek_timestamp = {};
}

void RemoteVideoSink::ThreadData::notify_clock_resumed() const
{
    m_delegates.notify_clock_resumed();
}

void RemoteVideoSink::ThreadData::transmit_seek(InvalidateHeldData invalidate_held_data) const
{
    m_delegates.transmit_seek(invalidate_held_data);
}

void RemoteVideoSink::ThreadData::transmit_time_reader(MediaTimeReader const& time_reader) const
{
    m_delegates.transmit_time_reader(time_reader);
}

Vector<NonnullRefPtr<VideoFrame>> RemoteVideoSink::ThreadData::frames_lent_after_holding(u64 newest_received_lend_id, AK::Duration timestamp) const
{
    // A frame lent again for an earlier seek is lent twice.
    Vector<NonnullRefPtr<VideoFrame>> frames;
    {
        MutexLocker locker { m_lent_frames_mutex };
        for (auto const& [lend_id, frame] : m_lent_frames) {
            if (lend_id <= newest_received_lend_id)
                continue;
            if (!frames.contains_slow(frame))
                frames.append(frame);
        }
    }
    quick_sort(frames, [](auto const& a, auto const& b) { return a->timestamp() < b->timestamp(); });
    if (frames.is_empty() || timestamp < frames.first()->timestamp() || timestamp >= frames.last()->conservative_end())
        return {};

    // The frame still in the input is lent again by taking it from the input.
    frames.remove_all_matching([&](auto const& frame) {
        return frame->end() <= timestamp || frame.ptr() == m_peeked_frame.ptr();
    });
    return frames;
}

void RemoteVideoSink::ThreadData::seek_upstream(AK::Duration timestamp, InvalidateHeldData invalidate_held_data, u64 newest_received_lend_id)
{
    MutexLocker locker { m_mutex };
    auto direct_seek_timestamp = m_direct_seek_timestamp;
    m_direct_seek_timestamp = {};
    m_frames_to_lend_again.clear();
    m_actual_seek_id++;

    auto seek_was_given_directly = direct_seek_timestamp == timestamp && invalidate_held_data == InvalidateHeldData::No;
    if (!seek_was_given_directly) {
        if (invalidate_held_data == InvalidateHeldData::No)
            m_frames_to_lend_again = frames_lent_after_holding(newest_received_lend_id, timestamp);
        if (m_frames_to_lend_again.is_empty() && m_input)
            m_input->seek(timestamp, invalidate_held_data);
    }
    m_wake_pending = true;
    m_wait_condition.broadcast();
}

bool RemoteVideoSink::ThreadData::consumer_may_satisfy_seek_locally(AK::Duration timestamp) const
{
    Optional<AK::Duration> earliest_timestamp;
    Optional<AK::Duration> latest_end;
    auto include_frame = [&](VideoFrame const& frame) {
        if (!earliest_timestamp.has_value() || frame.timestamp() < earliest_timestamp.value())
            earliest_timestamp = frame.timestamp();
        if (!latest_end.has_value() || frame.conservative_end() > latest_end.value())
            latest_end = frame.conservative_end();
    };
    {
        MutexLocker locker { m_lent_frames_mutex };
        for (auto const& [lend_id, frame] : m_lent_frames)
            include_frame(*frame);
    }
    if (m_peeked_frame)
        include_frame(*m_peeked_frame);
    if (!earliest_timestamp.has_value() || timestamp < earliest_timestamp.value())
        return false;
    if (timestamp < latest_end.value())
        return true;
    return m_published_status.has_value() && m_published_status->seek_id == m_actual_seek_id && m_published_status->status == PipelineStatus::EndOfStream;
}

void RemoteVideoSink::ThreadData::seek_input_directly(AK::Duration timestamp)
{
    MutexLocker locker { m_mutex };
    if (m_input == nullptr || m_direct_seek_timestamp.has_value() || consumer_may_satisfy_seek_locally(timestamp))
        return;
    // Pumping stops until the consumer's request for this seek arrives, so that no frame of the new position is
    // stamped with the seek before it.
    m_direct_seek_timestamp = timestamp;
    m_input->seek(timestamp, InvalidateHeldData::No);
}

void RemoteVideoSink::ThreadData::start_input()
{
    RefPtr<VideoProducer> input;
    {
        MutexLocker locker { m_mutex };
        input = m_input;
    }
    if (input)
        input->start();
}

void RemoteVideoSink::ThreadData::release_frame(u64 lend_id)
{
    // The frame is dropped outside the lock, since releasing its storage may wake its decoder.
    Optional<NonnullRefPtr<VideoFrame>> released_frame;
    {
        MutexLocker locker { m_lent_frames_mutex };
        released_frame = m_lent_frames.take(lend_id);
    }
    if (!released_frame.has_value())
        return;
    released_frame.clear();
    wake_pump_thread();
}

void RemoteVideoSink::ThreadData::wake_pump_thread()
{
    MutexLocker locker { m_mutex };
    m_wake_pending = true;
    m_wait_condition.broadcast();
}

void RemoteVideoSink::ThreadData::request_exit()
{
    RefPtr<VideoProducer> input;
    {
        MutexLocker locker { m_mutex };
        m_exit_requested = true;
        input = move(m_input);
        m_frames_to_lend_again.clear();
        m_wake_pending = true;
        m_wait_condition.broadcast();
    }
    if (input)
        input->set_wake_handler(nullptr);
}

void RemoteVideoSink::ThreadData::stop()
{
    request_exit();
    MutexLocker locker { m_mutex };
    while (!m_pump_thread_exited)
        m_wait_condition.wait();
}

bool RemoteVideoSink::ThreadData::has_lent_frames() const
{
    MutexLocker locker { m_lent_frames_mutex };
    return !m_lent_frames.is_empty();
}

void RemoteVideoSink::ThreadData::wait_on_pump_thread(MutexLocker<Mutex>&)
{
    while (!m_wake_pending && !m_exit_requested)
        m_wait_condition.wait();
    m_wake_pending = false;
}

void RemoteVideoSink::ThreadData::publish_status(PublishedStatus status)
{
    {
        MutexLocker locker { m_mutex };
        if (m_published_status == status)
            return;
        m_published_status = status;
    }
    m_delegates.send_status(status.status, status.seek_id);
}

void RemoteVideoSink::ThreadData::lend_frame(NonnullRefPtr<VideoFrame> const& frame, u32 seek_id)
{
    u64 lend_id = 0;
    {
        MutexLocker locker { m_lent_frames_mutex };
        lend_id = m_next_lend_id++;
        m_lent_frames.set(lend_id, frame);
    }

    m_delegates.send_frame({
        .lend_id = lend_id,
        .seek_id = seek_id,
        .metadata = VideoFrameMetadata::for_frame(*frame),
        .pixels = frame->storage()->pixels(),
    });
}

void RemoteVideoSink::ThreadData::pump_thread_loop()
{
    // The seek ID a frame was lent under, while it has yet to be consumed from the input.
    Optional<u32> lent_frame_seek_id;

    while (true) {
        if (lent_frame_seek_id.has_value()) {
            {
                MutexLocker locker { m_mutex };
                m_peeked_frame = nullptr;
                if (m_exit_requested)
                    break;
                if (m_actual_seek_id != lent_frame_seek_id.value()) {
                    // A seek was applied since the peek, so the input's head is re-peeked; if unchanged, it is lent
                    // again under the new stamp and the consumer drops the old copy.
                    lent_frame_seek_id = {};
                    continue;
                }
                if (m_direct_seek_timestamp.has_value()) {
                    wait_on_pump_thread(locker);
                    continue;
                }
                if (m_input)
                    m_input->consume();
            }
            publish_status({ PipelineStatus::Pending, lent_frame_seek_id.value() });
            lent_frame_seek_id = {};
            continue;
        }

        u32 seek_id = 0;
        Vector<NonnullRefPtr<VideoFrame>> frames_to_lend_again;
        VideoProducerOutput output;
        {
            MutexLocker locker { m_mutex };
            if (m_exit_requested)
                break;
            seek_id = m_actual_seek_id;
            frames_to_lend_again = move(m_frames_to_lend_again);
            if (frames_to_lend_again.is_empty()) {
                auto may_lend_another_frame = [&] {
                    MutexLocker lent_frames_locker { m_lent_frames_mutex };
                    return m_lent_frames.size() < MAX_LENT_FRAME_COUNT;
                }();
                if (m_input == nullptr || !may_lend_another_frame || m_direct_seek_timestamp.has_value()) {
                    wait_on_pump_thread(locker);
                    continue;
                }
                output = m_input->peek();
                if (output.status == PipelineStatus::HaveData)
                    m_peeked_frame = output.frame;
            }
        }

        if (!frames_to_lend_again.is_empty()) {
            for (auto const& frame : frames_to_lend_again)
                lend_frame(frame, seek_id);
            publish_status({ PipelineStatus::Pending, seek_id });
            continue;
        }

        if (output.status == PipelineStatus::HaveData) {
            VERIFY(output.frame);
            lend_frame(*output.frame, seek_id);
            lent_frame_seek_id = seek_id;
            continue;
        }

        publish_status({ output.status, seek_id });
        MutexLocker locker { m_mutex };
        wait_on_pump_thread(locker);
    }

    MutexLocker locker { m_mutex };
    m_peeked_frame = nullptr;
    m_pump_thread_exited = true;
    m_wait_condition.broadcast();
}

}
