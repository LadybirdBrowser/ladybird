/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/MediaTime.h>

#include "MediaTimeProvider.h"

namespace Media {

class GenericTimeProvider final : public MediaTimeProvider {
public:
    static ErrorOr<NonnullRefPtr<GenericTimeProvider>> try_create();
    virtual ~GenericTimeProvider() override;

    virtual MediaTimeReader time_reader() const override;
    virtual void resume() override;
    virtual void pause() override;
    virtual void seek(AK::Duration) override;
    virtual void set_playback_rate(float) override;

private:
    GenericTimeProvider(MediaTimeWriter, MediaTimeReader);

    MediaTimeWriter m_time_writer;
    MediaTimeReader m_time_reader;
    bool m_playing { false };
    float m_playback_rate { 1.0f };
};

}
