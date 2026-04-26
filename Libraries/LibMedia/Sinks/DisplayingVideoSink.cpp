/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Demuxer.h>
#include <LibMedia/MediaTimeProvider.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
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

DisplayingVideoSink::~DisplayingVideoSink() = default;

void DisplayingVideoSink::set_time_provider(NonnullRefPtr<MediaTimeProvider> const& provider)
{
    m_time_provider = provider;
}

void DisplayingVideoSink::verify_track(Track const& track) const
{
    if (m_producer == nullptr)
        return;
    VERIFY(m_track.has_value());
    VERIFY(m_track.value() == track);
}

void DisplayingVideoSink::set_producer(Track const& track, RefPtr<DecodedVideoProducer> const& producer)
{
    verify_track(track);
    m_track = track;
    m_producer = producer;
    if (producer != nullptr)
        producer->start();
}

RefPtr<DecodedVideoProducer> DisplayingVideoSink::producer(Track const& track) const
{
    verify_track(track);
    return m_producer;
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
    if (m_producer == nullptr)
        return DisplayingVideoSinkUpdateResult::NoChange;
    if (m_pause_updates)
        return DisplayingVideoSinkUpdateResult::NoChange;

    auto current_time = m_time_provider->current_time();
    auto result = DisplayingVideoSinkUpdateResult::NoChange;
    if (m_has_new_current_frame) {
        result = DisplayingVideoSinkUpdateResult::NewFrameAvailable;
        m_has_new_current_frame = false;
    }

    auto last_pull_status = PipelineStatus::Pending;
    while (true) {
        if (m_next_frame == nullptr) {
            last_pull_status = m_producer->pull(m_next_frame);
            if (last_pull_status != PipelineStatus::HaveData)
                break;
            VERIFY(m_next_frame != nullptr);
        }
        if (m_next_frame->timestamp() > current_time)
            break;
        m_current_frame = m_next_frame.release_nonnull();
        result = DisplayingVideoSinkUpdateResult::NewFrameAvailable;
    }

    auto effective_status = last_pull_status;
    if (m_next_frame != nullptr)
        effective_status = PipelineStatus::HaveData;
    dispatch_state_if_changed(effective_status);

    return result;
}

void DisplayingVideoSink::prepare_current_frame_for_next_update()
{
    auto update_result = update();
    if (update_result == DisplayingVideoSinkUpdateResult::NewFrameAvailable)
        m_has_new_current_frame = true;
}

RefPtr<VideoFrame> DisplayingVideoSink::current_frame()
{
    return m_current_frame;
}

void DisplayingVideoSink::pause_updates()
{
    m_pause_updates = true;
}

void DisplayingVideoSink::resume_updates()
{
    m_next_frame = nullptr;
    m_current_frame = nullptr;
    m_pause_updates = false;
    m_has_new_current_frame = true;
    prepare_current_frame_for_next_update();
}

}
