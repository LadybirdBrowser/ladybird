/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/PlaybackStates/PlaybackStateHandler.h>

namespace Media {

class EndedStateHandler final : public PlaybackStateHandler {
public:
    EndedStateHandler(PlaybackManager& manager, bool was_playing);
    virtual ~EndedStateHandler() override;

    virtual void on_enter() override;
    virtual void on_exit() override { }

    virtual AK::Duration current_time() const override;

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

    // Continues playback as it was before the end, for an owner that never observed the end.
    void resume_from_position_before_end();
    // Puts the pipeline at the duration, where an owner that observed the end expects a newly attached sink to be.
    void move_pipeline_to_end();

private:
    bool m_was_playing { false };
    AK::Duration m_position_before_end;
    bool m_pipeline_is_at_end { false };
};

}
