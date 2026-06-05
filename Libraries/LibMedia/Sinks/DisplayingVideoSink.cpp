/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/MediaTimeProvider.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMedia/VideoFrame.h>

#include "DisplayingVideoSink.h"

namespace Media {

ErrorOr<NonnullRefPtr<DisplayingVideoSink>> DisplayingVideoSink::try_create(NonnullRefPtr<MediaTimeProvider> const& time_provider, PipelineStateChangeHandler on_state_changed)
{
    return TRY(try_make_ref_counted<DisplayingVideoSink>(time_provider, move(on_state_changed)));
}

DisplayingVideoSink::DisplayingVideoSink(NonnullRefPtr<MediaTimeProvider> const& time_provider, PipelineStateChangeHandler on_state_changed)
    : m_time_provider(time_provider)
    , m_on_state_changed(move(on_state_changed))
{
}

DisplayingVideoSink::~DisplayingVideoSink()
{
    if (m_input != nullptr)
        m_input->set_wake_handler(nullptr);
}

void DisplayingVideoSink::set_time_provider(NonnullRefPtr<MediaTimeProvider> const& provider)
{
    m_time_provider = provider;
}

void DisplayingVideoSink::consume_moved_position_signals(PipelineStatus& status)
{
    while ((status = m_input->status()) == PipelineStatus::MovedPosition) {
        m_input->pull(m_next_frame);
        VERIFY(m_next_frame == nullptr);
        m_current_frame.clear();
        m_seek_status = SeekStatus::FrameInvalidated;
    }
    VERIFY(status != PipelineStatus::MovedPosition);
}

ErrorOr<void> DisplayingVideoSink::connect_input(NonnullRefPtr<VideoProducer> const& input)
{
    VERIFY(m_input == nullptr);
    m_input = input;
    input->set_wake_handler([this, input] {
        auto status = PipelineStatus::Pending;
        consume_moved_position_signals(status);
        if (!resolves_seek(status))
            return;
        dispatch_state_if_changed(status);
    });
    input->seek(m_time_provider->current_time());
    input->start();
    return {};
}

void DisplayingVideoSink::disconnect_input(NonnullRefPtr<VideoProducer> const& input)
{
    VERIFY(m_input == input);
    m_input->set_wake_handler(nullptr);
    m_input = nullptr;
}

static AK::Duration conservative_frame_end(VideoFrame& frame)
{
    return frame.timestamp() + frame.duration().scaled_by(3, 2);
}

void DisplayingVideoSink::seek(AK::Duration timestamp)
{
    m_seek_id++;
    m_last_dispatched_status = PipelineStatus::Pending;

    auto can_resolve_seek_within_cached_frames = [&] {
        if (m_seek_status != SeekStatus::None)
            return false;
        auto available_start = AK::Duration::max();
        auto available_end = AK::Duration::min();
        auto include_frame = [&](RefPtr<VideoFrame> const& frame) {
            if (frame == nullptr)
                return;
            available_start = min(available_start, frame->timestamp());
            available_end = max(available_end, conservative_frame_end(*frame));
        };
        include_frame(m_current_frame);
        include_frame(m_next_frame);

        return timestamp >= available_start && timestamp < available_end;
    }();

    if (can_resolve_seek_within_cached_frames) {
        Core::deferred_invoke([self = NonnullRefPtr(*this), seek_id = m_seek_id] {
            if (self->m_seek_id != seek_id)
                return;
            self->dispatch_state_if_changed(PipelineStatus::HaveData);
            VERIFY(self->m_seek_status == SeekStatus::None);
        });
        return;
    }

    if (m_input != nullptr)
        m_input->seek(timestamp);
    if (m_seek_status == SeekStatus::None)
        m_seek_status = SeekStatus::InProgress;
}

void DisplayingVideoSink::dispatch_state_if_changed(PipelineStatus status)
{
    if (status == m_last_dispatched_status)
        return;
    m_last_dispatched_status = status;
    if (m_on_state_changed)
        m_on_state_changed(status);
}

DisplayingVideoSinkUpdateResult DisplayingVideoSink::update()
{
    if (m_input == nullptr)
        return DisplayingVideoSinkUpdateResult::NoChange;

    auto current_time = m_time_provider->current_time();
    auto result = DisplayingVideoSinkUpdateResult::NoChange;

    auto last_status = PipelineStatus::Pending;
    while (true) {
        if (m_next_frame == nullptr || m_seek_status != SeekStatus::None)
            consume_moved_position_signals(last_status);
        if (m_next_frame == nullptr) {
            if (last_status != PipelineStatus::HaveData)
                break;
            m_input->pull(m_next_frame);
            VERIFY(m_next_frame != nullptr);
            if (m_seek_status == SeekStatus::FrameInvalidated)
                m_current_frame.clear();
        }
        if (resolves_seek(last_status))
            m_seek_status = SeekStatus::None;
        if (m_seek_status != SeekStatus::None)
            break;
        if (m_next_frame->timestamp() > current_time)
            break;
        m_current_frame = m_next_frame.release_nonnull();
        result = DisplayingVideoSinkUpdateResult::NewFrameAvailable;
    }

    // Dispatch the new state with a deferred invoke to avoid reentrancy. This prevents a seek from resolving while
    // an update is being processed.
    Core::deferred_invoke([self = NonnullRefPtr(*this), last_status] {
        self->dispatch_state_if_changed(last_status);
    });

    return result;
}

RefPtr<VideoFrame> DisplayingVideoSink::current_frame()
{
    return m_current_frame;
}

}
