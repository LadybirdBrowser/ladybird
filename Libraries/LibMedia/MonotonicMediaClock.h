/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Export.h>
#include <LibMedia/MediaTime.h>

#include "MediaClock.h"

namespace Media {

class MEDIA_API MonotonicMediaClock final : public MediaClock {
public:
    static ErrorOr<NonnullRefPtr<MonotonicMediaClock>> try_create();
    virtual ~MonotonicMediaClock() override;

    virtual MediaTimeReader time_reader() const override;
    virtual void resume() override;
    virtual void pause() override;
    virtual void seek(AK::Duration) override;
    virtual void set_playback_rate(float) override;

private:
    MonotonicMediaClock(MediaTimeWriter, MediaTimeReader);

    MediaTimeWriter m_time_writer;
    MediaTimeReader m_time_reader;
    bool m_playing { false };
    float m_playback_rate { 1.0f };
};

}
