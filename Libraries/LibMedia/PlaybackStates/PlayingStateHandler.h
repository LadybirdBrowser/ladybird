/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Forward.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/Forward.h>

namespace Media {

class PlayingStateHandler final : public PlaybackStateHandler {
public:
    PlayingStateHandler(PlaybackManager& manager);
    virtual ~PlayingStateHandler() override;

    virtual void on_enter() override
    {
        manager().m_clock->resume();
        update_unticked_end_of_stream_timer();
    }
    virtual void on_exit() override
    {
        manager().m_clock->pause();
    }

    virtual void play() override { }
    virtual void pause() override;

    virtual bool is_playing() override
    {
        return true;
    }
    virtual PlaybackState state() override
    {
        return PlaybackState::Playing;
    }
    virtual AvailableData available_data() override
    {
        return AvailableData::Future;
    }

    virtual void on_pipeline_status_changed(PipelineStatus) override;

private:
    void update_unticked_end_of_stream_timer();

    RefPtr<Core::Timer> m_unticked_end_of_stream_timer;
};

}
