/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoFrameHandle.h>

#include "DisplayingVideoSink.h"

namespace Media {

ErrorOr<NonnullRefPtr<DisplayingVideoSink>> DisplayingVideoSink::try_create(MediaTimeReader time_reader)
{
    return TRY(try_make_ref_counted<DisplayingVideoSink>(move(time_reader)));
}

DisplayingVideoSink::DisplayingVideoSink(MediaTimeReader time_reader)
    : m_time_reader(move(time_reader))
{
}

DisplayingVideoSink::~DisplayingVideoSink()
{
    if (m_input != nullptr)
        m_input->set_wake_handler(nullptr);
}

void DisplayingVideoSink::set_time_reader(MediaTimeReader time_reader)
{
    m_time_reader = move(time_reader);
}

void DisplayingVideoSink::set_state_change_handler(PipelineStateChangeHandler on_state_changed)
{
    m_on_state_changed = move(on_state_changed);
}

void DisplayingVideoSink::set_resize_handler(PipelineResizeHandler on_resize)
{
    m_on_resize = move(on_resize);
}

ErrorOr<void> DisplayingVideoSink::connect_input(NonnullRefPtr<VideoProducer> const& input)
{
    VERIFY(m_input == nullptr);
    m_input = input;
    input->set_wake_handler([this] {
        if (m_input->peek().status == PipelineStatus::Suspended) {
            m_next_frame = nullptr;
            dispatch_state_if_changed(PipelineStatus::Suspended);
            return;
        }
        update(MonotonicTime::now());
    });
    input->seek(m_time_reader.current_time());
    input->start();
    return {};
}

void DisplayingVideoSink::disconnect_input(NonnullRefPtr<VideoProducer> const& input)
{
    VERIFY(m_input == input);
    m_input->set_wake_handler(nullptr);
    m_input = nullptr;
}

void DisplayingVideoSink::seek(AK::Duration timestamp)
{
    m_seek_id++;
    m_last_dispatched_status = PipelineStatus::Pending;

    auto can_resolve_seek_within_cached_frames = [&] {
        if (m_seek_status != SeekStatus::None)
            return false;
        if (m_cached_frames_are_discontinuous)
            return false;
        auto available_start = AK::Duration::max();
        auto available_end = AK::Duration::min();
        auto include_frame = [&](RefPtr<VideoFrame> const& frame) {
            if (frame == nullptr)
                return;
            available_start = min(available_start, frame->timestamp());
            available_end = max(available_end, frame->conservative_end());
        };
        include_frame(m_current_frame);
        include_frame(m_next_frame);

        if (timestamp < available_start)
            return false;
        if (timestamp < available_end)
            return true;
        return m_input != nullptr && m_input->peek().status == PipelineStatus::EndOfStream;
    }();

    if (can_resolve_seek_within_cached_frames) {
        update(MonotonicTime::now());
        return;
    }

    if (m_input != nullptr)
        m_input->seek(timestamp);
    if (m_seek_status == SeekStatus::None)
        m_seek_status = SeekStatus::InProgress;
    m_next_frame = nullptr;
}

void DisplayingVideoSink::dispatch_state_if_changed(PipelineStatus status)
{
    if (status == m_last_dispatched_status)
        return;
    m_last_dispatched_status = status;
    if (m_on_state_changed)
        m_on_state_changed(status);
}

DisplayingVideoSinkUpdateResult DisplayingVideoSink::update(MonotonicTime now)
{
    if (m_input == nullptr)
        return {};

    auto time_state = m_time_reader.time_state(now);
    auto current_time = time_state.time;
    auto new_frame_available = false;

    auto last_status = PipelineStatus::Pending;
    auto seek_resolved_during_this_update = false;
    while (true) {
        if (m_next_frame == nullptr) {
            auto output = m_input->peek();
            last_status = output.status;
            if (output.frame == nullptr) {
                if (is_terminal(last_status) && m_seek_status == SeekStatus::InProgress) {
                    m_seek_status = SeekStatus::None;
                    new_frame_available = true;
                }
                break;
            }
            m_next_frame = move(output.frame);
            m_input->consume();
        }
        if (m_seek_status != SeekStatus::None && resolves_seek(last_status)) {
            m_current_frame.clear();
            m_seek_status = SeekStatus::None;
            seek_resolved_during_this_update = true;
            new_frame_available = true;
        }
        if (m_seek_status != SeekStatus::None)
            break;
        if (m_next_frame->timestamp() > current_time)
            break;

        if (auto frame_size = m_next_frame->size(); frame_size != m_last_dispatched_size) {
            m_last_dispatched_size = frame_size;
            if (m_on_resize)
                m_on_resize(frame_size);
        }

        // Skip presenting frames that are late, unless we're resolving a seek. Seeks should always resolve with a
        // frame regardless of whether it is late.
        if (!seek_resolved_during_this_update && current_time > m_next_frame->conservative_end()) {
            auto next = m_input->peek();
            last_status = next.status;
            if (!is_terminal(last_status)) {
                if (last_status == PipelineStatus::HaveData) {
                    m_next_frame = move(next.frame);
                    m_input->consume();
                    m_cached_frames_are_discontinuous = true;
                    continue;
                }
                break;
            }
        }
        m_current_frame = m_next_frame.release_nonnull();
        m_cached_frames_are_discontinuous = false;
        new_frame_available = true;
    }

    if (m_current_frame != nullptr && m_seek_status == SeekStatus::None) {
        AK::Duration current_frame_end;
        if (is_terminal(last_status))
            current_frame_end = m_current_frame->timestamp() + m_current_frame->duration();
        else
            current_frame_end = m_current_frame->conservative_end();
        if (current_time <= current_frame_end)
            last_status = PipelineStatus::HaveData;
    }

    if (last_status == PipelineStatus::Suspended) {
        m_next_frame = nullptr;
        m_seek_status = SeekStatus::InProgress;
        m_input->seek(current_time);
        last_status = PipelineStatus::Pending;
    }

    if (new_frame_available) {
        if (m_presented_frame_page.has_value())
            m_presented_frame_page->store(m_current_frame ? VideoFrameHandle::for_frame(*m_current_frame) : VideoFrameHandle {});
        if (m_on_present_needed)
            m_on_present_needed();
    }

    if (last_status != m_last_dispatched_status) {
        // Dispatch the new state with a deferred invoke to avoid reentrancy. This prevents a seek from resolving while
        // an update is being processed.
        Core::deferred_invoke([weak_self = make_weak_ref(), last_status, seek_id = m_seek_id] {
            auto self = weak_self.strong_ref();
            if (!self)
                return;
            if (seek_id != self->m_seek_id)
                return;
            self->dispatch_state_if_changed(last_status);
        });
    }

    return {
        .new_frame_available = new_frame_available,
        .may_require_updates = !is_terminal(last_status) && (time_state.is_advancing || m_seek_status != SeekStatus::None),
    };
}

RefPtr<VideoFrame> DisplayingVideoSink::current_frame() const
{
    return m_current_frame;
}

}
