/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackManager.h>

namespace Media {

class EndedStateHandler final : public PlaybackStateHandler {
public:
    EndedStateHandler(PlaybackManager& manager)
        : PlaybackStateHandler(manager)
    {
    }
    virtual ~EndedStateHandler() override = default;

    virtual void on_enter() override
    {
        manager().m_time_provider->pause();
    }
    virtual void on_exit() override { }

    virtual AK::Duration current_time() const override
    {
        return manager().duration();
    }

    virtual void play() override { }
    virtual void pause() override { }

    virtual bool is_playing() override
    {
        return false;
    }
    virtual PlaybackState state() override
    {
        return PlaybackState::Ended;
    }
    virtual AvailableData available_data() override
    {
        return AvailableData::Current;
    }

    virtual void on_pipeline_status_changed(PipelineStatus) override { }
};

}
