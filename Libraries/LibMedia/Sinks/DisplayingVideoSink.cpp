/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/Producers/VideoProducer.h>
#include <LibMediaCodecs/VideoFrame.h>

#include "DisplayingVideoSink.h"

namespace Media {

ErrorOr<NonnullRefPtr<DisplayingVideoSink>> DisplayingVideoSink::try_create(MediaTimeReader time_reader)
{
    return TRY(try_make_ref_counted<DisplayingVideoSink>(move(time_reader)));
}

DisplayingVideoSink::DisplayingVideoSink(MediaTimeReader time_reader)
    : m_time_reader(move(time_reader))
{
    m_reported_freshness = freshness();
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
        auto output = m_input->peek();
        m_input_checkpoint = output.checkpoint;
        if (output.status == PipelineStatus::Suspended) {
            m_next_frame = nullptr;
            // A ticking sink's update resumes the input if the time is moving, and reports the suspension otherwise.
            if (is_ticking()) {
                update(MonotonicTime::now());
                return;
            }
            m_latest_status = PipelineStatus::Suspended;
            dispatch_latest_state_if_changed();
            report_freshness_if_changed();
            return;
        }
        // Update the sink to dispatch a status if we're still resolving a seek or waiting for data. If we're not, avoid
        // the update so that we don't count dropped frames due to the decoder waiting for room while we're not
        // consuming frames.
        auto status_may_change = [&] {
            if (m_seek_status != SeekStatus::None || m_seek_resolution_is_pending_frame_at_target)
                return true;
            return is_waiting_for_data(m_latest_status);
        }();
        if (!is_ticking() && !status_may_change)
            return;
        update(MonotonicTime::now());
    });
    if (m_ticking != VideoSinkTicking::Undecided) {
        start_input();
        report_freshness_if_changed();
    }
    return {};
}

void DisplayingVideoSink::start_input()
{
    VERIFY(m_ticking != VideoSinkTicking::Undecided);
    if (!is_ticking()) {
        m_input->suspend();
        return;
    }
    m_next_frame = nullptr;
    m_latest_status = PipelineStatus::Pending;
    m_seek_status = SeekStatus::CatchingUp;
    m_seek_resolution_is_pending_frame_at_target = true;
    m_input->seek(m_time_reader.current_time());
    m_input->start();
}

void DisplayingVideoSink::disconnect_input(NonnullRefPtr<VideoProducer> const& input)
{
    VERIFY(m_input == input);
    m_input->set_wake_handler(nullptr);
    m_input = nullptr;
}

void DisplayingVideoSink::seek(AK::Duration timestamp, InvalidateHeldData invalidate_held_data)
{
    begin_seek(timestamp, invalidate_held_data, SeekStatus::InProgress);
    report_freshness_if_changed();
}

void DisplayingVideoSink::begin_seek(AK::Duration timestamp, InvalidateHeldData invalidate_held_data, SeekStatus seek_status)
{
    VERIFY(seek_status != SeekStatus::None);
    m_seek_id++;
    m_latest_status = PipelineStatus::Pending;
    m_last_dispatched_status = PipelineStatus::Pending;
    m_seek_resolution_is_pending_frame_at_target = true;

    // The input is seeked to the current time when ticking starts. Don't seek and possibly unsuspend the input yet.
    if (m_ticking == VideoSinkTicking::Undecided)
        return;

    // If the sink isn't ticking, don't unsuspend the input unnecessarily. Let it catch up later. Data that the seek
    // invalidates may be what the current frame shows, so the seek replaces the frame then.
    if (seek_status == SeekStatus::InProgress && invalidate_held_data == InvalidateHeldData::No && !is_ticking() && m_input != nullptr && m_input->peek().status == PipelineStatus::Suspended) {
        m_next_frame = nullptr;
        m_seek_status = SeekStatus::None;
        m_seek_resolution_is_pending_frame_at_target = false;
        m_latest_status = PipelineStatus::Suspended;
        defer_latest_status_dispatch();
        return;
    }

    auto can_resolve_seek_within_cached_frames = [&] {
        if (invalidate_held_data == InvalidateHeldData::Yes)
            return false;
        if (m_seek_status != SeekStatus::None)
            return false;
        if (m_cached_frames_are_discontinuous)
            return false;
        if (m_input != nullptr && m_input->peek().status == PipelineStatus::Suspended)
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
        m_input->seek(timestamp, invalidate_held_data);
    m_seek_status = seek_status;
    m_next_frame = nullptr;
}

void DisplayingVideoSink::dispatch_latest_state_if_changed()
{
    if (m_latest_status == m_last_dispatched_status && m_input_checkpoint == m_last_dispatched_checkpoint)
        return;
    m_last_dispatched_status = m_latest_status;
    m_last_dispatched_checkpoint = m_input_checkpoint;
    if (m_on_state_changed)
        m_on_state_changed(m_latest_status, m_input_checkpoint);
}

// Dispatches with a deferred invoke to avoid reentrancy. This prevents a seek from resolving while an update or a seek
// is being processed.
void DisplayingVideoSink::defer_latest_status_dispatch()
{
    if (m_latest_status == m_last_dispatched_status && m_input_checkpoint == m_last_dispatched_checkpoint)
        return;
    Core::deferred_invoke([weak_self = make_weak_ref(), seek_id = m_seek_id] {
        auto self = weak_self.strong_ref();
        if (!self)
            return;
        if (seek_id != self->m_seek_id)
            return;
        self->dispatch_latest_state_if_changed();
    });
}

DisplayingVideoSinkUpdateResult DisplayingVideoSink::update(MonotonicTime now)
{
    if (m_input == nullptr || m_ticking == VideoSinkTicking::Undecided)
        return {};
    if (is_ticking())
        m_has_updated_since_ticking_began = true;

    auto time_state = m_time_reader.time_state(now);
    auto current_time = time_state.time;
    auto new_frame_available = false;
    u64 passed_frame_count = 0;
    auto last_passed_frame_was_late = false;
    Optional<AK::Duration> last_passed_timestamp;
    if (m_current_frame)
        last_passed_timestamp = m_current_frame->timestamp();

    auto last_status = PipelineStatus::Pending;
    auto seek_resolved_during_this_update = false;
    while (true) {
        if (m_next_frame == nullptr) {
            auto output = m_input->peek();
            last_status = output.status;
            m_input_checkpoint = output.checkpoint;
            if (output.frame == nullptr) {
                if (is_terminal(last_status) && m_seek_status != SeekStatus::None) {
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
            last_passed_timestamp.clear();
            m_seek_status = SeekStatus::None;
            seek_resolved_during_this_update = true;
            new_frame_available = true;
        }
        if (m_seek_status != SeekStatus::None)
            break;
        if (m_next_frame->timestamp() > current_time)
            break;

        if (last_passed_timestamp.has_value() && last_passed_timestamp.value() > m_next_frame->timestamp()) {
            m_next_frame.clear();
            continue;
        }

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
            m_input_checkpoint = next.checkpoint;
            if (!is_terminal(last_status)) {
                // A passed frame stays on screen until the next one replaces it.
                auto next_frame_is_due = last_status == PipelineStatus::HaveData && next.frame->timestamp() <= current_time;
                if (next_frame_is_due) {
                    if (!m_seek_resolution_is_pending_frame_at_target) {
                        passed_frame_count++;
                        last_passed_frame_was_late = true;
                    }
                    last_passed_timestamp = m_next_frame->timestamp();
                    m_next_frame = move(next.frame);
                    m_input->consume();
                    m_cached_frames_are_discontinuous = true;
                    continue;
                }
                if (last_status != PipelineStatus::HaveData)
                    break;
            }
        }
        m_current_frame = m_next_frame.release_nonnull();
        m_cached_frames_are_discontinuous = false;
        new_frame_available = true;
        last_passed_timestamp = m_current_frame->timestamp();
        if (m_seek_resolution_is_pending_frame_at_target)
            passed_frame_count = 1;
        else
            passed_frame_count++;
        last_passed_frame_was_late = false;
    }

    if (last_passed_frame_was_late)
        m_frame_counts.dropped += passed_frame_count;
    else
        m_frame_counts.presented += passed_frame_count;

    auto status_to_dispatch = last_status;

    if (m_current_frame != nullptr && m_seek_status == SeekStatus::None) {
        auto current_frame_end = m_current_frame->conservative_end();
        if (is_terminal(last_status) || m_seek_resolution_is_pending_frame_at_target)
            current_frame_end = m_current_frame->end();
        if (current_time < current_frame_end)
            status_to_dispatch = PipelineStatus::HaveData;
    }

    if (m_next_frame != nullptr && m_next_frame->timestamp() > current_time)
        status_to_dispatch = PipelineStatus::HaveData;

    if (resolves_seek(status_to_dispatch))
        m_seek_resolution_is_pending_frame_at_target = false;

    if (last_status == PipelineStatus::Suspended) {
        auto time_is_changing = time_state.is_advancing || m_seek_status != SeekStatus::None;
        if (time_is_changing) {
            m_next_frame = nullptr;
            if (m_seek_status == SeekStatus::None)
                m_seek_status = SeekStatus::CatchingUp;
            m_seek_resolution_is_pending_frame_at_target = true;
            m_input->seek(current_time);
            status_to_dispatch = PipelineStatus::Pending;
        } else {
            status_to_dispatch = PipelineStatus::Suspended;
        }
    }

    if (new_frame_available && m_on_present_needed)
        m_on_present_needed();

    m_latest_status = status_to_dispatch;
    defer_latest_status_dispatch();

    auto may_require_updates = !is_terminal(status_to_dispatch) && (time_state.is_advancing || m_seek_status != SeekStatus::None);

    // Whoever drives our updates stops once we no longer require them, so ask for a present to restart them.
    auto updates_were_stopped = !m_last_update_may_require_updates;
    m_last_update_may_require_updates = may_require_updates;
    if (may_require_updates && updates_were_stopped && !new_frame_available && m_on_present_needed)
        m_on_present_needed();

    report_freshness_if_changed();
    return {
        .new_frame_available = new_frame_available,
        .may_require_updates = may_require_updates,
    };
}

void DisplayingVideoSink::notify_clock_resumed()
{
    if (is_ticking())
        update(MonotonicTime::now());
}

void DisplayingVideoSink::set_ticking(bool ticking)
{
    auto new_ticking = ticking ? VideoSinkTicking::Yes : VideoSinkTicking::No;
    auto previous_ticking = exchange(m_ticking, new_ticking);
    if (previous_ticking == new_ticking)
        return;
    if (ticking)
        m_has_updated_since_ticking_began = false;
    if (previous_ticking == VideoSinkTicking::Undecided) {
        if (m_input != nullptr)
            start_input();
    } else if (ticking) {
        catch_up_to_clock();
    }
    report_freshness_if_changed();
}

DisplayingVideoSink::Freshness DisplayingVideoSink::freshness() const
{
    return { .is_ticking = is_ticking(), .is_catching_up = is_catching_up(), .frame_is_up_to_date = frame_is_up_to_date() };
}

void DisplayingVideoSink::report_freshness_if_changed()
{
    auto freshness = this->freshness();
    if (freshness == m_reported_freshness)
        return;
    m_reported_freshness = freshness;
    if (m_on_freshness_changed)
        m_on_freshness_changed();
}

void DisplayingVideoSink::catch_up_to_clock()
{
    if (m_ticking == VideoSinkTicking::Undecided || m_seek_status != SeekStatus::None)
        return;
    auto current_time = m_time_reader.current_time();

    // Don't request a seek to the same frame, or we'll wake upstream while suspended for no reason.
    if (m_current_frame != nullptr && m_current_frame->contains_timestamp(current_time))
        return;

    begin_seek(current_time, InvalidateHeldData::No, SeekStatus::CatchingUp);
    report_freshness_if_changed();
}

RefPtr<VideoFrame> DisplayingVideoSink::current_frame() const
{
    return m_current_frame;
}

}
