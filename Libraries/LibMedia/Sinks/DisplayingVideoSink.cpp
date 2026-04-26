/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
        m_input->set_state_changed_handler(nullptr);
}

void DisplayingVideoSink::set_time_provider(NonnullRefPtr<MediaTimeProvider> const& provider)
{
    m_time_provider = provider;
}

ErrorOr<void> DisplayingVideoSink::connect_input(NonnullRefPtr<VideoProducer> const& input)
{
    VERIFY(m_input == nullptr);
    m_input = input;
    input->set_state_changed_handler([this](PipelineStatus status) {
        if (!m_seek_pending_display_update)
            return;
        if (is_waiting_for_data(status))
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
    m_input->set_state_changed_handler(nullptr);
    m_input = nullptr;
}

void DisplayingVideoSink::seek(AK::Duration timestamp)
{
    if (m_input != nullptr)
        m_input->seek(timestamp);
    m_next_frame.clear();
    m_last_dispatched_status = PipelineStatus::Pending;
    m_seek_pending_display_update = true;
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

    auto last_pull_status = PipelineStatus::Pending;
    while (true) {
        if (m_next_frame == nullptr) {
            last_pull_status = m_input->pull(m_next_frame);
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

    if (m_seek_pending_display_update && !is_waiting_for_data(effective_status)) {
        if (result != DisplayingVideoSinkUpdateResult::NewFrameAvailable) {
            m_current_frame.clear();
            result = DisplayingVideoSinkUpdateResult::NewFrameAvailable;
        }
        m_seek_pending_display_update = false;
    }

    dispatch_state_if_changed(effective_status);

    return result;
}

RefPtr<VideoFrame> DisplayingVideoSink::current_frame()
{
    return m_current_frame;
}

}
