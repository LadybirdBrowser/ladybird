/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Providers/VideoDataProvider.h>
#include <LibMedia/VideoDecoder.h>
#include <LibThreading/Mutex.h>

#include "DisplayingVideoSink.h"

namespace Media {

ErrorOr<NonnullRefPtr<DisplayingVideoSink>> DisplayingVideoSink::try_create(NonnullRefPtr<MediaTimeProvider> const& time_provider)
{
    return TRY(try_make_ref_counted<DisplayingVideoSink>(time_provider));
}

DisplayingVideoSink::DisplayingVideoSink(NonnullRefPtr<MediaTimeProvider> const& time_provider)
    : m_time_provider(time_provider)
{
}

DisplayingVideoSink::~DisplayingVideoSink() = default;

void DisplayingVideoSink::verify_track(Track const& track) const
{
    if (m_provider == nullptr)
        return;
    VERIFY(m_track.has_value());
    VERIFY(m_track.value() == track);
}

void DisplayingVideoSink::set_provider(Track const& track, RefPtr<VideoDataProvider> const& provider)
{
    verify_track(track);
    m_track = track;
    m_provider = provider;
}

RefPtr<VideoDataProvider> DisplayingVideoSink::provider(Track const& track) const
{
    verify_track(track);
    return m_provider;
}

DisplayingVideoSinkUpdateResult DisplayingVideoSink::update()
{
    auto current_time = m_time_provider->current_time();
    auto result = DisplayingVideoSinkUpdateResult::NoChange;
    Threading::MutexLocker locker { m_mutex };
    while (true) {
        if (!m_next_frame.is_valid()) {
            m_next_frame = m_provider->retrieve_frame();
            if (!m_next_frame.is_valid())
                break;
        }
        if (m_next_frame.timestamp() > current_time)
            break;
        m_current_frame = m_next_frame.release_image();
        result = DisplayingVideoSinkUpdateResult::NewFrameAvailable;
    }
    return result;
}

RefPtr<Gfx::Bitmap> DisplayingVideoSink::current_frame()
{
    Threading::MutexLocker locker { m_mutex };
    return m_current_frame;
}

}
