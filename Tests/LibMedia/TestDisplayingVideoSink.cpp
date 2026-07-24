/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/MonotonicMediaClock.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoFramePool.h>
#include <LibTest/TestCase.h>

#include "TestMediaCommon.h"

namespace {

class ScriptedVideoProducer final : public Media::VideoProducer {
public:
    static NonnullRefPtr<ScriptedVideoProducer> create()
    {
        return adopt_ref(*new ScriptedVideoProducer());
    }

    void append_output(RefPtr<Media::VideoFrame> frame, Media::PipelineStatus status)
    {
        m_outputs.append({ move(frame), status });
    }

    virtual void start() override { }
    virtual Media::VideoProducerOutput peek() override
    {
        if (m_outputs.is_empty())
            return { nullptr, Media::PipelineStatus::Pending };
        return m_outputs.first();
    }
    virtual void consume() override { m_outputs.take_first(); }
    virtual void set_wake_handler(Media::PipelineWakeHandler handler) override { m_wake_handler = move(handler); }
    virtual void seek(AK::Duration) override { }

private:
    ScriptedVideoProducer() = default;

    Vector<Media::VideoProducerOutput> m_outputs;
    Media::PipelineWakeHandler m_wake_handler;
};

NonnullRefPtr<Media::VideoFrame> create_test_frame(Media::VideoFramePool& pool, AK::Duration timestamp, AK::Duration duration)
{
    static constexpr Gfx::IntSize frame_size { 2, 2 };
    static constexpr u8 bit_depth = 8;
    auto subsampling = Media::Subsampling(true, true);

    auto plane_sizes = MUST(Gfx::YUVData::plane_sizes(frame_size, bit_depth, subsampling));
    auto acquired = pool.try_acquire(plane_sizes.total).release_value();
    auto yuv_data = MUST(Gfx::YUVData::create(frame_size, bit_depth, subsampling, Media::CodingIndependentCodePoints {},
        acquired.bytes.slice(0, plane_sizes.y),
        acquired.bytes.slice(plane_sizes.y, plane_sizes.u),
        acquired.bytes.slice(plane_sizes.y + plane_sizes.u, plane_sizes.v)));
    auto slot = MUST(pool.try_adopt_acquired_slot(acquired));

    return make_ref_counted<Media::VideoFrame>(timestamp, duration, Gfx::Size<u32>(frame_size), bit_depth, yuv_data, move(slot));
}

}

TEST_CASE(seek_resolving_frame_is_presented_even_when_stale)
{
    never_destroyed_event_loop();

    auto pool = MUST(Media::VideoFramePool::create());
    auto clock = MUST(Media::MonotonicMediaClock::try_create());
    clock->seek(AK::Duration::from_milliseconds(1000));

    auto sink = MUST(Media::DisplayingVideoSink::try_create(clock->time_reader()));
    auto producer = ScriptedVideoProducer::create();
    MUST(sink->connect_input(producer));

    // The producer resolves the seek with a frame that covers the target, but whose presentation
    // interval has already passed relative to the clock.
    sink->seek(AK::Duration::from_milliseconds(900));
    producer->append_output(create_test_frame(*pool, AK::Duration::from_milliseconds(900), AK::Duration::from_milliseconds(33)), Media::PipelineStatus::HaveData);

    auto result = sink->update(MonotonicTime::now());
    EXPECT_EQ(result, Media::DisplayingVideoSinkUpdateResult::NewFrameAvailable);

    auto current_frame = sink->current_frame();
    EXPECT(current_frame != nullptr);
    EXPECT_EQ(current_frame->timestamp(), AK::Duration::from_milliseconds(900));
}
